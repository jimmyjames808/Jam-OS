/* volumes: the disks and their filesystem services (bin/fat), and the
 * mounts they make: /esp and /data from the disk Jam OS booted from, /usbN
 * from every other stick (disk.c, fsvc.c). A process in bootfs
 * (bin/volumes) that init starts right after devmgr, with the server ends
 * of its control channel, the ESP channel (shell mode) and the DISKS
 * channel, and a channel for the desktop's notices (<volumes.h>: the
 * protocol, the roles, and the rules its clients follow). Until M12's
 * storage split (track S1b) all of this was devmgr's.
 *
 * A disk reaches volumes on the DISKS channel: devmgr asks each running
 * usb-storage for a `storage` channel of volumes' own (svc.connect) and
 * hands it over (DISKS_ATTACH), and says when the device has gone
 * (DISKS_GONE). Test disks come on the control channel (TEST_DISK).
 * volumes starts a filesystem service per partition that holds one (fat,
 * in a job of its own under volumes'), supervises it by the rule of a
 * service that outlives its process (supervise.c, spare.c: its `fs`
 * channel, keeper and state outlive each instance; a warm spare is
 * promoted for a restart), and hands the mounts' channels to init
 * (VOLUMES_MOUNTS, mounts.c).
 *
 * Its words: "bootdisk=0x<id>" (the MBR disk id of the disk the machine
 * booted from: disk.c's boot-disk check) and "nospare" (no warm spare fat:
 * a restart starts a process).
 *
 * It runs until every client end of init's control channel is gone (init
 * closes its own at the end of a test run), or SHUTDOWN (a reboot): then
 * every writable mount is synced, the spare and every filesystem service
 * stop, and it exits 0 if nothing went wrong meanwhile (a boot-disk
 * service that crashed or was given up on, one that didn't end cleanly).
 * When it dies otherwise, init starts it again with fresh channels: every
 * mount went with its job, and comes back once devmgr has told the new
 * volumes of its disks. */
#include "disk.h"

struct vol vols[MAX_VOLS];
unsigned   nvols, problems;
handle_t   port;
struct notice_box notices;
uint32_t   boot_mbr_id;
uint64_t   volumes_started;
bool       stopping, quiet;

/* The supervision module's lines (<supervise.h>) go where say's do. */
static void sup_sink(bool report_it, const char *line)
{
    say(report_it, "%s", line);
}

void say(bool report_it, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    if (report_it)
        jam_debug_report(buf, (uint64_t)n);
    else
        jam_debug_write(buf, (uint64_t)n);
}

bool in_bootfs(const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    return bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, &size) == OK;
}

/* Wait for one packet (or the next due restart or deadline) and act on it. */
static status_t wait_event(void)
{
    struct port_packet pkt;
    uint64_t deadline = sup_next_deadline();
    if (disk_next_deadline() < deadline)
        deadline = disk_next_deadline();
    if (mounts_next_deadline() < deadline)
        deadline = mounts_next_deadline();
    if (sup_job_next_deadline() < deadline)
        deadline = sup_job_next_deadline();
    status_t st = jam_port_wait(port, deadline, &pkt);
    if (st != OK)
        return st;
    if (chans_packet(pkt.key))
        return OK;   /* chans_serve reads what came */
    if ((pkt.key & KEY_FAT) && KEY_INDEX(pkt.key) < nvols) {
        sup_died(&vols[KEY_INDEX(pkt.key)], KEY_GEN(pkt.key));
    } else if ((pkt.key & KEY_EVENTS) && KEY_INDEX(pkt.key) < nvols) {
        struct vol *b = &vols[KEY_INDEX(pkt.key)];
        if (b->client_key == pkt.key)
            fs_answers(b);
    } else if (pkt.key & KEY_DISKS) {
        disks_event(pkt.key);
    } else if (pkt.key & KEY_DISK) {
        disk_key(pkt.key);
    } else if (pkt.key == KEY_SPARE) {
        spare_event();
    } else if (pkt.key & KEY_KEEP) {
        spare_keep_event(pkt.key);
    }
    return OK;
}

/* Serve until the life channel's clients are all gone (ERR_PEER_CLOSED)
 * or something fails (its status). */
static status_t run(void)
{
    for (;;) {
        status_t st = chans_serve();
        if (stopping)
            return ERR_PEER_CLOSED;   /* SHUTDOWN: as if every client had gone */
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = chans_arm();
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = wait_event();
        if (st != OK && st != ERR_TIMED_OUT)
            return st;
        sup_run_due();
        disk_run_due();
        mounts_run_due();
        sup_job_run_due();
    }
}

/* Every client is gone, or SHUTDOWN: no more restarts. Every writable
 * mount is synced while its disk still works, then the spare and every
 * filesystem service stop (killed: what they hold is on the stick, and
 * their disks are about to go with devmgr's drivers anyway). True if
 * nothing went wrong while running. */
static bool stop_all(void)
{
    bool ok = true;
    unsigned stopped = 0;
    stopping = true;
    spare_stop();      /* the warm spare, and the `block` channel it had */
    disk_sync_all();   /* before anything under the filesystems goes */
    for (unsigned i = 0; i < nvols; i++) {
        struct vol *b = &vols[i];
        sup_reset(b);
        if (b->proc) {
            fs_stop(b, true, true);
            stopped++;
        }
        fs_close_client(b);
        fs_ctl_close(b);
        fs_kept_release(b);
    }
    disk_close_all();
    if (!sup_job_settle())
        ok = false;   /* said which, and counted below */
    problems += sup_job_unclean();   /* jobs not empty in the end (supervise.h) */
    if (problems) {
        say(true, "volumes: %u filesystem problem(s) while running (crashes, give-ups: see "
            "above)", problems);
        ok = false;
    }
    say(false, "volumes: %u filesystem service(s) stopped; exiting", stopped);
    return ok;
}

/* "0x1234abcd" (or without 0x) as a number; it stops at the first
 * character that isn't a hex digit. */
static uint32_t hex32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint32_t v = 0;
    for (int n = 0; n < 8; n++, s++) {
        char c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            break;
        v = v << 4 | (uint32_t)d;
    }
    return v;
}

int main(int argc, char **argv)
{
    sup_log("volumes", sup_sink);   /* first: everything after may say something */
    volumes_started = now();
    bool nospare = false;
    for (int i = 1; i < argc; i++) {
        nospare |= !strcmp(argv[i], "nospare");
        if (!strncmp(argv[i], "bootdisk=", 9))
            boot_mbr_id = hex32(argv[i] + 9);
    }
    if (!chans_init()) {
        say(true, "volumes: no control channel in the startup message");
        return 1;
    }
    status_t st = jam_port_create(&port);
    if (st != OK) {
        say(true, "volumes: no port (%s)", status_str(st));
        return 1;
    }
    notice_init(&notices, startup_handle(VOLUMES_SR_NOTIFY));
    spare_init(!nospare);
    handle_t disks_end = startup_handle(VOLUMES_SR_DISKS);
    say(false, "volumes: serving%s", disks_end ? "" : " (no disks channel)");
    disks_set(disks_end);

    st = run();
    bool ok = true;
    if (st != ERR_PEER_CLOSED) {
        say(true, "volumes: serving failed (%s)", status_str(st));
        ok = false;
    }
    if (!stop_all())
        ok = false;
    return ok ? 0 : 1;
}
