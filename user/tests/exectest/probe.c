/* exectest's and execwide's body (exectest.h): `probe` reports what the
 * program was given, `launch` starts another through /svc/exec. */
#include <os.h>
#include "exectest.h"

#define LAUNCH_WAIT (60 * NS_PER_S)

static bool has_svc(const char *name)
{
    handle_t ch;
    if (svc_open(name, &ch) != OK)
        return false;
    jam_handle_close(ch);
    return true;
}

static bool has_mount(const char *path)
{
    handle_t ch;
    if (ns_channel(path, &ch) != OK)
        return false;
    jam_handle_close(ch);
    return true;
}

/* Does /data take a new file? (Removed again at once.) */
static bool data_writable(void)
{
    struct jfile f;
    if (file_open(EXT_PROBE_FILE, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f) != OK)
        return false;
    file_close(&f);
    (void)fs_unlink(EXT_PROBE_FILE);
    return true;
}

static int probe(void)
{
    unsigned bits = 0;
    struct handle_info hi;
    handle_t root = startup_handle(SR_RESOURCE);
    bits |= has_svc(SVC_EXEC) ? EXT_EXEC : 0;
    bits |= has_mount("/data") ? EXT_DATA : 0;
    bits |= (bits & EXT_DATA) && data_writable() ? EXT_DATA_W : 0;
    bits |= has_mount("/boot") ? EXT_BOOT : 0;
    if (root && jam_handle_info(root, &hi) == OK) {
        bits |= EXT_ROOT;
        bits |= hi.rights & RIGHT_ROOT_SYSINFO ? EXT_SYSINFO : 0;
        bits |= hi.rights & RIGHT_TRANSFER ? EXT_ROOT_TX : 0;
    }
    bits |= has_svc(SVC_DEVMGR) ? EXT_DEVMGR : 0;
    bits |= has_svc(SVC_VOLUMES) ? EXT_VOLUMES : 0;
    bits |= startup_handle(SR_SIGNALS) ? EXT_SIGNALS : 0;
    printf("exectest: given 0x%x\n", bits);
    return (int)bits;
}

static int launch(int argc, char **argv)
{
    static const char *const env[] = EXT_ENV;
    handle_t proc, job, sig;
    int step;
    status_t st = exectest_start(argv[2], argc - 2, (const char *const *)argv + 2, env, &proc,
                                 &job, &sig, &step);
    if (st != OK) {
        printf("exectest: %s: step %d: %s\n", argv[2], step, status_str(st));
        return EXT_LAUNCH_ERR + step;
    }
    struct process_info info;
    st = spawn_wait(proc, LAUNCH_WAIT, &info);
    if (st != OK)
        jam_job_kill(job);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(sig);
    if (st != OK)
        return EXT_LAUNCH_ERR + 6;
    return info.killed ? EXT_LAUNCH_ERR + 7 : (int)info.exit_code;
}

int exectest_main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "probe"))
        return probe();
    if (argc >= 3 && !strcmp(argv[1], "launch"))
        return launch(argc, argv);
    printf("usage: %s probe | launch <path> [args...]\n", argc ? argv[0] : "exectest");
    return 64;
}
