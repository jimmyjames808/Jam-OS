/* init's shell mode: the desktop's apps (initctl.launch: the compositor's
 * search box, and the shell's channel). Only the programs <deskapps.h>
 * lists, from the boot image: the compositor names an app and init picks
 * the program, so the compositor's channel can start nothing else (no
 * path, no arguments, nothing from /data).
 *
 * Each app gets what the shell would give it when typed at a prompt (its
 * own list, <wants.h>: the services and mounts it names, read-only or
 * writable as it says, and the root's rights it asks for), and
 * /svc/wayland whether its list says so or not (a desktop app is a
 * window); but no console (it was started from no terminal) and no
 * SR_STDOUT: what it prints goes to the log. It runs in a job of its own
 * under init's, watched on the loop's port (KEY_APPS + its slot): when its
 * process ends, the job is killed (anything it left running goes) and its
 * slot is free. init never starts one again: closing its window is the end
 * of it. At most DESKAPPS_RUNNING at once. */
#include <deskapps.h>
#include <wants.h>
#include "init.h"

#define GRANTS_MAX (WANTS_MAX + 2)   /* the list's, /svc/wayland, the end */

struct app {
    handle_t proc, job;   /* 0: a free slot */
    char name[16];
};

static struct app apps[DESKAPPS_RUNNING];
static handle_t port;

void apps_init(handle_t loop_port)
{
    port = loop_port;
}

/* The program for desktop app `cmd`, or NULL: not one init launches. */
static const char *app_path(const char *cmd)
{
#define PATH_OF(c, path, name, about, letter, tint)                        \
    if ((path) && !strcmp(cmd, (c)))                                       \
        return (path);
    DESKAPPS(PATH_OF)
#undef PATH_OF
    return NULL;
}

/* The root's rights its list asks for, as the shell gives them. */
static rights_t root_rights(uint32_t want)
{
    rights_t r = RIGHT_DUPLICATE | RIGHT_WAIT | RIGHT_INSPECT;
    r |= want & WANT_RIGHT_KLOG ? RIGHT_ROOT_KLOG : 0;
    r |= want & WANT_RIGHT_SYSINFO ? RIGHT_ROOT_SYSINFO : 0;
    r |= want & WANT_RIGHT_CLOCK ? RIGHT_ROOT_CLOCK : 0;
    r |= want & WANT_RIGHT_DEBUG ? RIGHT_ROOT_DEBUG : 0;
    return r;
}

/* Start path as app `cmd` in slot a: OK with its process and job there. */
static status_t start(struct app *a, const char *cmd, const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    static struct wants w;   /* one start at a time: init has one loop */
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, path, &data, &size) != OK)
        return ERR_NOT_FOUND;
    if (wants_read(data, size, &w) != OK)
        memset(&w, 0, sizeof(w));   /* checked at build time: a bad one gives nothing */
    const char *grants[GRANTS_MAX];
    unsigned n = 0;
    bool wayland = false;
    for (unsigned i = 0; i < w.n; i++) {
        grants[n++] = w.grant[i];
        wayland |= !strcmp(w.grant[i], "/svc/" SVC_WAYLAND);
    }
    if (!wayland)
        grants[n++] = "/svc/" SVC_WAYLAND;
    grants[n] = NULL;
    struct spawn_handle x[1];
    unsigned nx = 0;
    if (w.rights) {
        handle_t root = services_root_with(root_rights(w.rights));
        if (root)
            x[nx++] = (struct spawn_handle){ SR_RESOURCE, root };
    }
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &a->job);
    if (st != OK) {
        for (unsigned i = 0; i < nx; i++)
            jam_handle_close(x[i].h);
        return st;
    }
    const char *argv[] = { path };
    struct spawn_args sa = {
        .path = path, .argc = 1, .argv = argv, .job = a->job, .extra = x, .nextra = nx,
        .ns = grants,
    };
    st = spawn(&sa, &a->proc);   /* the extras go, whatever happens */
    if (st == OK)
        st = jam_port_bind(port, a->proc, KEY_APPS + (uint64_t)(a - apps), SIG_TERMINATED,
                           PORT_BIND_ONCE);
    if (st != OK) {
        if (a->proc) {
            jam_job_kill(a->job);
            jam_handle_close(a->proc);
        }
        jam_handle_close(a->job);
        *a = (struct app){ 0 };
        return st;
    }
    snprintf(a->name, sizeof(a->name), "%s", cmd);
    return OK;
}

status_t apps_launch(const char *cmd, uint64_t *koid)
{
    size_t len = strnlen(cmd, 16);
    if (!len || len == 16)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < len; i++)
        if (!((cmd[i] >= 'a' && cmd[i] <= 'z') || (cmd[i] >= '0' && cmd[i] <= '9') ||
              cmd[i] == '-'))
            return ERR_INVALID_ARGS;
    const char *path = app_path(cmd);
    if (!path)
        return ERR_NOT_FOUND;   /* not a desktop app: nothing else is started this way */
    unsigned i = 0;
    while (i < DESKAPPS_RUNNING && apps[i].proc)
        i++;
    if (i == DESKAPPS_RUNNING)
        return ERR_NO_RESOURCES;
    status_t st = start(&apps[i], cmd, path);
    struct process_info info;
    if (st == OK && jam_process_get_info(apps[i].proc, &info) == OK)
        *koid = info.koid;
    printf("init: launch %s: %s", cmd, status_str(st));
    if (st == OK)
        printf(" (%s, process %lu)", path, (unsigned long)*koid);
    printf("\n");
    return st;
}

void apps_event(unsigned i)
{
    struct app *a = &apps[i];
    struct process_info info;
    if (i >= DESKAPPS_RUNNING || !a->proc ||
        spawn_wait(a->proc, 0, &info) == ERR_TIMED_OUT)
        return;   /* an old packet */
    if (info.killed)
        printf("init: %s was killed\n", a->name);
    else
        printf("init: %s ended with code %ld\n", a->name, (long)info.exit_code);
    struct job_info ji;   /* whatever it left running (an empty job: no kill, no log line) */
    if (jam_job_get_info(a->job, &ji) != OK || ji.used[JOB_LIMIT_THREADS])
        jam_job_kill(a->job);
    jam_handle_close(a->proc);
    jam_handle_close(a->job);
    *a = (struct app){ 0 };
}
