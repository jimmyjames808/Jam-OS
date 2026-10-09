/* volumes' own pieces: main.c (startup, the event loop, the stop),
 * chans.c (its channels: reading requests, what each channel may ask),
 * request.c (the answers), supervise.c (a filesystem service's start, its
 * end, its restart with backoff or giving up), disk.c (the disks devmgr
 * tells of and the test disks: which one is the boot disk) and fsvc.c
 * (their filesystem services; disk.h between the two), mounts.c (the
 * list of mounts, its generation and the watches told of each change),
 * spare.c (what outlives a filesystem service's process: its keeper and
 * state VMO, and the warm spare fat). The protocols are
 * abi/idl/volumes.idl; the startup roles <volumes.h>. Until M12's storage
 * split all of this was devmgr's (user/services/devmgr/), and its log
 * lines keep their words with the prefix "volumes: ". */
#pragma once

#include <idl/volumes.h>
#include <notice.h>
#include <os.h>
#include <supervise.h>
#include <volumes.h>

#define MAX_VOLS  32   /* filesystem services: every partition of every disk */
/* A filesystem service's own channels (FAT_SR_BLOCK, FAT_SR_CTL): devmgr's
 * rights for a driver's channels, so it can't pass them on. */
#define FAT_CHAN_RIGHTS (RIGHT_WAIT | RIGHT_INSPECT | RIGHT_READ | RIGHT_WRITE)
/* How long a filesystem service gets to end by itself when asked to stop,
 * before its job is killed (devmgr's driver rule). */
#define STOP_WAIT (15 * NS_PER_S)

/* Port keys: a filesystem service's SIG_TERMINATED (vols index and start
 * generation, so a stale packet is recognised). */
#define KEY_FAT            (1ull << 32)
#define KEY_OF(i, gen)     (KEY_FAT | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* A filesystem service answered on its `fs` channel (fs.stat's probe):
 * vols index and start generation. */
#define KEY_EVENTS         (1ull << 33)
#define KEY_EV_OF(i, gen)  (KEY_EVENTS | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* The DISKS channel has something to read, or closed: its generation. */
#define KEY_DISKS          (1ull << 34)
/* A disk's `storage` channel has something to read, or closed: disks
 * slot and its generation. */
#define KEY_DISK           (1ull << 35)
#define KEY_DISK_OF(i, gen) (KEY_DISK | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* One of volumes' channels has something to read, or closed (chans.c):
 * its slot and the slot's generation. */
#define KEY_CHAN           (1ull << 36)
#define KEY_CHAN_OF(i, gen) (KEY_CHAN | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
/* The warm spare fat's process terminated (spare.c). */
#define KEY_SPARE          (1ull << 37)
/* A filesystem service's keep channel has something to read (spare.c):
 * vols index and the keeper's generation. */
#define KEY_KEEP           (1ull << 38)
#define KEY_KEEP_OF(i, gen) (KEY_KEEP | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
#define KEY_INDEX(k)       ((uint32_t)((k) >> 16) & 0xffffu)
#define KEY_GEN(k)         ((uint32_t)(k) & 0xffffu)

/* What a channel of volumes' speaks (chans.c): the control channels
 * `volumes` (and svc.connect); init's ESP channel `volesp` alone
 * (<volumes.h> "Trust"). */
enum level {
    LEVEL_CONTROL,
    LEVEL_ESP,
};

/* A filesystem service on one partition of a disk: bin/fat, supervised by
 * the rule of a service that outlives its process. path NULL: a free
 * slot. */
struct vol {
    const char         *path;       /* FAT_PATH; NULL: a free slot */
    bool                test;       /* a test disk's: its deaths and giving up are expected */
    /* The service while it runs. */
    handle_t            job, proc;  /* its job and process, 0 while none runs */
    handle_t            client;     /* our end of its `fs` channel (the watches get duplicates) */
    uint64_t            client_key; /* the port watches client for answers under it (0: not) */
    handle_t            serve;      /* the service's end, kept across its restarts */
    uint32_t            gen;        /* bumped at every start: in its port key */
    bool                killed;     /* ended by volumes.kill */
    status_t            last;       /* the last start's status */
    /* Supervision. */
    uint32_t            state;      /* VOLUMES_SUP_* */
    uint64_t            restart_at; /* VOLUMES_SUP_RESTARTING: when */
    struct sup_window   win;        /* the restart window (<supervise.h>) */
    char                name[32];   /* its process name: "fat-data" */
    uint32_t            disk;       /* its disks slot + 1; 0: none */
    uint8_t             part;       /* the partition it serves (storage.idl's index) */
    handle_t            ctl;        /* our end of its `fsctl` channel (0: none) */
    /* On a disk that isn't the boot disk (disk.c): a /usbN mount. */
    bool                other;      /* it is one: never given FAT_ARG_FORMAT */
    bool                rw;         /* its `block` channel is opened read-write (`mount -w`;
                                     * the boot disk's ESP: init's volesp.open_writable) */
    bool                ready;      /* it answered its first fs.stat: a mount */
    uint8_t             usbn;       /* the N of /usbN */
    uint32_t            probe;      /* that fs.stat's transaction id */
    /* What outlives its process (spare.c). Its `fs` channel is kept too:
     * client and serve stay volumes' from one instance to the next
     * (fs_serve_end), so a restart is not a new channel. */
    struct sup_kept    *kept;       /* its keeper and state VMO; NULL until its first start */
    bool                deliberate; /* the restart due is a deliberate kill's: not counted */
    uint64_t            kill_at;    /* when volumes.kill asked for its end (uptime ns; 0: none) */
    uint64_t            ended_at;   /* when its last instance's end was seen (uptime ns) */
    uint32_t            promoted;   /* restarts that promoted the warm spare, since boot */
    /* Bumped with every new `fs` channel (fs_new_channel): a new mount
     * for the watches (mounts.c). A restart on the kept channel doesn't
     * move it: fat keeps its views (and open files) across its deaths. */
    uint32_t            chan_gen;
};

extern struct vol vols[MAX_VOLS];
extern unsigned   nvols;
extern handle_t   port;
/* The desktop's notices (VOLUMES_SR_NOTIFY; none under `nocomp`). */
extern struct notice_box notices;
/* Problems for the exit code: boot-disk filesystem services that crashed
 * or were given up on, and services that didn't end cleanly. */
extern unsigned   problems;
/* The argument "bootdisk=0x<id>": the MBR disk id of the disk the machine
 * booted from (the loader's, passed on by the kernel and init); 0: not
 * known. disk.c takes the Jam OS disk with that id as the boot disk. */
extern uint32_t   boot_mbr_id;
extern uint64_t   volumes_started;   /* when volumes started (uptime, ns) */
/* volumes.stop was asked, or the last control client left: the loop
 * stops, and nothing that happens from then on is news to the desktop. */
extern bool       stopping;
/* The disks being forgotten are no news either (the DISKS channel
 * closed: devmgr ended, and its disks with it). */
extern bool       quiet;
/* The limits of a filesystem service's job (16 MiB, 256 handles, 16
 * threads, 1 MiB queued: devmgr's for a driver), the spare's too. */
extern const struct sup_limit fat_limits[];
extern const unsigned         fat_nlimits;

void say(bool report_it, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool in_bootfs(const char *path);
/* The next message on h didn't fit (ERR_BUFFER_TOO_SMALL: n bytes, nh
 * handles): take it off the queue anyway, unanswered, and close its
 * handles. (chans.c) */
void discard(handle_t h, uint32_t n, uint32_t nh);

/* main.c. Every client is gone, or volumes.stop: no more restarts, every
 * writable mount synced, the spare and every filesystem service stopped.
 * Once only (later calls do nothing). True if nothing went wrong while
 * running. */
bool     stop_all(void);

/* chans.c. Take the channels init gave (the startup message); false if
 * there is none to live by (no control channel). */
bool     chans_init(void);
/* Answer what is queued on every channel: ERR_SHOULD_WAIT once all are
 * drained, ERR_PEER_CLOSED once the clients of the channel volumes lives
 * by are all gone, else a read's failure. */
status_t chans_serve(void);
/* Watch every channel that isn't watched (ONCE): ERR_SHOULD_WAIT when all
 * are, else a bind's failure. */
status_t chans_arm(void);
/* A port packet: true if it was a channel's. */
bool     chans_packet(uint64_t key);

/* request.c: the handlers of `volumes` (the control channels) and
 * `volesp` (init's ESP channel). */
extern const struct volumes_ops volumes_ops;
extern const struct volesp_ops  volesp_ops;

/* supervise.c: a filesystem service's process. */
/* Start b's service (state RUNNING on success): its job, its handles
 * (fs_handles: a new `block` channel; fs_serve_end: the kept `fs`
 * channel's server end), its process. */
status_t fs_spawn(struct vol *b);
/* Kill b's service's whole job. Returns once it is dead. */
void     fs_kill(struct vol *b);
/* Let go of a dead service's process and job (and its port binding). */
void     fs_forget(struct vol *b);
/* Stop b's running service: close our `fs` client end, wait for it to
 * return (kill it if it doesn't, or at once if `kill`), forget it. True if
 * it ended with exit 0 by itself (or `excused`) and its job is empty. */
bool     fs_stop(struct vol *b, bool kill, bool excused);
/* Close b's client end (and stop watching it). */
void     fs_close_client(struct vol *b);
/* b's process (start generation `gen`) terminated. */
void     sup_died(struct vol *b, uint32_t gen);
/* volumes.kill: b's instance killed on purpose (*koid: its process's),
 * and its restart scheduled. ERR_NOT_FOUND: no instance runs. */
status_t sup_kill(struct vol *b, uint64_t *koid);
/* Start every restart that is due. */
void     sup_run_due(void);
/* The earliest due restart, or DEADLINE_NEVER. */
uint64_t sup_next_deadline(void);
/* Forget b's supervision: no restart is due any more, the kept channel
 * end goes; the restart history is cleared. */
void     sup_reset(struct vol *b);

/* disk.c: the disks devmgr tells of (and volumes.test_disk's), and the
 * filesystem services of the boot disk. */
#define PART_ESP  0u   /* partition 1 of the boot disk: its ESP, read-only, at /esp */
#define PART_DATA 1u   /* partition 2: its data partition, at /data */
/* The DISKS channel (server end, consumed; HANDLE_INVALID: none): the old
 * one's disks are forgotten, without notices. */
void     disks_set(handle_t ch);
/* KEY_DISKS: what devmgr sent, or its end closed. */
void     disks_event(uint64_t key);
/* b (a filesystem service) has something to read on its channel: the
 * answers to what volumes asked without waiting. */
void     fs_answers(struct vol *b);
/* b's process started (supervise.c): it is a mount from now on, or once
 * it answers its first fs.stat. */
void     disk_started(struct vol *b);
/* b's process is gone (supervise.c): no mount until it is back, unless
 * its kept channel stays one. */
void     disk_stopped(struct vol *b);
/* A disk's KEY_DISK packet. */
void     disk_key(uint64_t key);
/* volumes.test_disk: ch (consumed) is a `storage` channel; *id: the disk's. */
status_t disk_test(handle_t ch, uint32_t *id);
/* b died, its process still held. True if that is the end of it (b
 * freed, no restart): the ESP's service ended before it answered the
 * boot-disk check, so the disk is not the boot disk; or a /usbN service
 * ended by itself with FAT_EXIT_NO_VOLUME (`no_volume`): the partition
 * holds no FAT volume and is left alone. */
bool     fs_check_ended(struct vol *b, bool no_volume);
/* FAT_ARG_FORMAT for the one service that may format a blank partition
 * (the boot disk's data partition), NULL for every other. */
const char *fs_format_arg(const struct vol *b);
/* For an instance that replaces one that ended: FAT_ARG_KILLED after a
 * deliberate kill, FAT_ARG_CRASHED after any other end; NULL for a first
 * start (fat counts only crashes against the request in progress). */
const char *fs_end_arg(const struct vol *b);
/* volumes.remount: b (a /usbN or /usbN-test mount) read-write or
 * read-only. ERR_NOT_FOUND: not such a mount; ERR_BAD_STATE: its service
 * isn't serving. */
status_t disk_remount(struct vol *b, bool writable);
/* volesp: the boot disk's ESP read-write (*out: a channel to its service,
 * which is no mount meanwhile) or read-only again (out unused).
 * ERR_NOT_FOUND: no boot disk with an ESP service; ERR_BAD_STATE: it
 * isn't serving. */
status_t disk_esp_write(bool writable, handle_t *out);
/* volumes.release_disk: disk id's mounts stopped in order and gone; the
 * disk isn't mounted again until it is attached again. ERR_NOT_FOUND: no
 * such disk. */
status_t disk_release(uint32_t id);
/* b's handles for its service: FAT_SR_BLOCK, a new `block` channel on its
 * partition, FAT_SR_CTL, and its state and keep channel.
 * ERR_PEER_CLOSED: the disk is gone. */
status_t fs_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* Close b's `fsctl` channel, if it has one. */
void     fs_ctl_close(struct vol *b);
/* The mount point b serves: "/data", "/esp-test", "/usb0". The string is
 * good until the next call. */
const char *fs_mount_path(const struct vol *b);
/* b won't run again: free its slot. */
void     fs_retire(struct vol *b);
/* The filesystem service of the mount called path (len bytes, no NUL:
 * "/data", "/usb0-test"), running or not, or NULL. */
struct vol *fs_find_mount(const char *path, uint32_t len);
/* b's FAT_SR_SERVE: a duplicate of the `fs` server end volumes keeps
 * across its restarts (a new channel only when there is none, or its
 * client end was closed). */
status_t fs_serve_end(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* A new `fs` channel for b (client and serve), its server end marked
 * "receives no handles" (no fs request carries one), so its queue can't
 * hold a channel end and it can always be handed to the next instance
 * (M12's D2). */
status_t fs_new_channel(struct vol *b, handle_t *client, handle_t *serve);
/* Does b keep its `fs` channel while no instance runs? Then it stays a
 * mount meanwhile, and calls on it wait for the next instance. */
bool     fs_channel_kept(const struct vol *b);
/* Every writable mount: fs.sync while its disk still works, SYNC_WAIT
 * each. The first failure (OK: none). */
status_t disk_sync_all(void);
/* Give up on what did not answer in time, and start the next /usbN
 * service that waits its turn. Called after every event. */
void     disk_run_due(void);
/* When disk_run_due has something to do next, or DEADLINE_NEVER. */
uint64_t disk_next_deadline(void);
/* volumes is stopping: each disk's `storage` channel closed. */
void     disk_close_all(void);

/* A mount: a running filesystem service of the boot disk, or of another
 * disk once its volume is mounted. */
struct mount {
    char     path[16];   /* the mount point */
    uint32_t vol;        /* vols index of its filesystem service */
    uint32_t gen;        /* that service's `fs` channel's generation (chan_gen): a new
                          * channel is a new mount, a restart on the kept one isn't */
    uint8_t  flags;      /* MOUNTS_READ_ONLY, MOUNTS_TEST */
};
/* The mounts as they are now into out (MAX_VOLS slots). Returns how many. */
unsigned disk_mounts(struct mount *out);

/* mounts.c: the list of mounts and the watches (volumes.watch, the
 * `mounts` events). */
/* Something that may have changed the mounts happened: if the list differs
 * from the one the watches were last told, the generation moves on and
 * each watch is told what changed. */
void     mounts_update(void);
/* volumes.watch: a new watch (*out: the watcher's end). It gets the list
 * once its end has gone out with the reply (mounts_list_new). */
status_t mounts_watch(handle_t *out);
/* After a reply went out: the new watches get the whole list. */
void     mounts_list_new(void);
/* volumes is stopping: every watch closes. */
void     mounts_close_all(void);

/* spare.c: a filesystem service that outlives its process
 * (docs/M11.6-PLAN.md): per service a keeper and a state VMO, and one
 * warm spare fat for every mount. */
/* `on`: keep a warm spare (no boot word `nospare`). */
void     spare_init(bool on);
/* Start b: for a `restart` after a death, the warm spare if one waits,
 * else (and for any other start) a new process (fs_spawn); either way
 * with its state and a new keep channel, whose keeper then hands it what
 * was kept. A restart is measured: the log says when it ran and when it
 * first answered. Errors as fs_spawn's. */
status_t fs_run(struct vol *b, bool restart);
/* b's state VMO (made the first time) and a new keep channel into x
 * (SR_STATE, SR_KEEP), for its next instance. One that can't be made is
 * said and left out: that instance starts fresh. */
void     fs_kept_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* The `block` channel opened in advance for b (the boot disk's /data)
 * into *out, if there is one and its disk still answers: true. */
bool     fs_block_prepared(const struct vol *b, handle_t *out);
/* b won't run again as it was (retired, given up on, stopped in order):
 * what its keeper kept closes (its clients see ERR_PEER_CLOSED), its state
 * VMO goes, and a `block` channel opened in advance for it. */
void     fs_kept_release(struct vol *b);
/* Start the warm spare (and open /data's `block` channel for it) when due. */
void     spare_due(void);
uint64_t spare_next_deadline(void);   /* DEADLINE_NEVER: nothing due */
/* The port's packets: KEY_SPARE (the spare ended), KEY_KEEP (a keep channel). */
void     spare_event(void);
void     spare_keep_event(uint64_t key);
/* SUPERVISION: is a warm spare waiting now? */
bool     spare_waits(void);
/* volumes is stopping: the spare goes, and the `block` channel it had. */
void     spare_stop(void);
