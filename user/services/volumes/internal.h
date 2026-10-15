/* volumes' own pieces: main.c (startup, the event loop, the stop),
 * chans.c (its channels: reading requests, what each channel may ask),
 * request.c (the answers), supervise.c (a filesystem service's start, its
 * end, its restart with backoff or giving up), disk.c (the disks devmgr
 * tells of and the test disks: which one is the boot disk) and fsvc.c
 * (their filesystem services; disk.h between the two), mounts.c (the
 * list of mounts, its generation and the watches told of each change),
 * spare.c (what outlives a filesystem service's process: its keeper and
 * state VMO, and the warm spare fat), calls.c (the loop's cooperative
 * tasks, and the calls they wait for). The protocols are
 * abi/idl/volumes.idl; the startup roles <volumes.h>. Until M12's storage
 * split all of this was devmgr's (user/services/devmgr/), and its log
 * lines keep their words with the prefix "volumes: ". */
#pragma once

#include <idl/volumes.h>
#include <jam/task.h>
#include <notice.h>
#include <os.h>
#include <supervise.h>
#include <volumes.h>

struct disk;

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
/* A filesystem service's `fsctl` channel has an answer, or closed: vols
 * index and the channel's generation (ctl_gen). */
#define KEY_FSCTL          (1ull << 39)
#define KEY_FSCTL_OF(i, gen) (KEY_FSCTL | (uint64_t)(i) << 16 | ((gen) & 0xffffu))
#define KEY_INDEX(k)       ((uint32_t)((k) >> 16) & 0xffffu)
#define KEY_GEN(k)         ((uint32_t)(k) & 0xffffu)

/* What a channel of volumes' speaks (chans.c): the control channels
 * `volumes` (and svc.connect); init's ESP channel `volesp` alone and its
 * install channel `volinstall` alone (<volumes.h> "Trust"). */
enum level {
    LEVEL_CONTROL,
    LEVEL_ESP,
    LEVEL_LIST,      /* `voldisks` alone (volumes.list_channel) */
    LEVEL_INSTALL,
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
    handle_t            serve;      /* the service's end, kept across its restarts */
    /* Our own `fs` channel to it (FAT_SR_OWN), never handed to anyone: our
     * stat probes and syncs go there (fs_asker), so they never wait behind
     * a client's requests on the shared one, and no client reading its
     * end can take their answers. Made and closed with client; 0 if it
     * couldn't be made (they go on client then). */
    handle_t            own;
    handle_t            own_serve;  /* its server end, kept across restarts as serve is */
    uint64_t            client_key; /* the port watches own and client for answers under it
                                     * (fs_watch_answers; 0: not) */
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
    uint32_t            ctl_gen;    /* bumped with every new one: in its port key */
    /* On a disk that isn't the boot disk (disk.c): a /usbN mount. */
    bool                other;      /* it is one: never given FAT_ARG_FORMAT */
    bool                rw;         /* its `block` channel is opened read-write (`mount -w`;
                                     * the boot disk's ESP: init's volesp.open_writable) */
    bool                ready;      /* it answered its first fs.stat: a mount */
    bool                nvme;       /* ... of /nvmeN (an NVMe drive's partition: never writable) */
    uint8_t             usbn;       /* the N of /usbN (or /nvmeN) */
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
    /* The tasks at work on it (calls.c). Each remembers `serial` and stops
     * when it moved: the slot was retired (and maybe used again) meanwhile. */
    uint32_t            serial;     /* bumped whenever the slot is retired */
    bool                starting;   /* a task starts its service (fs_start, a restart) */
    bool                stopping;   /* a task stops it in order: its end is that task's,
                                     * not supervision's (sup_died leaves it alone) */
    bool                busy;       /* a remount, an ESP write or a release is under way */
    bool                held;       /* init's installer holds it writable (install.c): no
                                     * mount for anyone else until it lets go */
    bool                spare_now;  /* its instance is the warm spare promoted ("fat-spare") */
    /* fs_start's: told how the start went (in the task, once it is known). */
    void              (*on_started)(struct disk *d, unsigned part, status_t st);
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
 * booted from (a stick: the loader's, passed on by the kernel and init);
 * 0: not known, or a GPT disk. Or "bootdisk=gpt:<GUID>": the GPT disk
 * GUID of the disk the machine booted from (an installed drive), as its
 * bytes on disk; zeros: not known, or an MBR disk. disk.c takes the Jam OS
 * disk named either way as the boot disk. */
extern uint32_t   boot_mbr_id;
extern uint8_t    boot_gpt[16];
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

/* main.c. Every client is gone, or volumes.stop: no more restarts, the
 * spare and every filesystem service stopped (killed: the mounts were
 * synced before, by the stop's task). Once only (later calls do nothing). True if
 * nothing went wrong while running. */
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
/* volumes.list_channel: a new channel that speaks `voldisks` alone (*out:
 * its client end). ERR_NO_RESOURCES: no free slot. */
status_t chans_add_list(handle_t *out);

/* request.c: the handlers of `volumes` (the control channels) and
 * `volesp` (init's ESP channel); the `later` ones start a task that
 * answers. */
extern const struct volumes_ops volumes_ops;
extern const struct volesp_ops  volesp_ops;
extern const struct voldisks_ops voldisks_ops;
extern const struct volinstall_ops volinstall_ops;
/* The stop (volumes.stop's, or with nobody to answer once every client
 * has gone): a task that syncs every writable mount, then stop_all. False
 * if it couldn't start (stop_all has run). */
bool     stop_start(struct idl_txn txn);
/* The stop's result: stop_all's, once its task has ended. */
bool     stop_result(void);

/* calls.c: the loop's cooperative tasks (<jam/task.h>), and the calls
 * they make to other processes: sent, then waited for in the task while
 * the loop goes on; the loop reads every answer off its channel and hands
 * a task's to it (call_offer). */
#define CALL_REP_MAX 192   /* the biggest answer a task waits for (storage's) */
struct call {
    handle_t     ch;        /* the channel it went on */
    uint32_t     txid;      /* its transaction id */
    bool         done;      /* answered (or its channel went: st) */
    status_t     st;        /* OK: rep and m hold the answer; ERR_PEER_CLOSED */
    _Alignas(8) uint8_t rep[CALL_REP_MAX];
    struct idl_msg m;       /* its handles are the caller's (the _result takes them) */
    struct call *next;      /* the calls waited for */
};
extern struct task_set *tasks;
bool     calls_init(void);
/* The next transaction id of a request written without waiting. */
uint32_t call_txid(void);
/* c, a request about to go on ch, is waited for from now on (its answer
 * can't be missed however long the task takes to wait): its txid is
 * c->txid. Then call_wait, or call_end if the send failed. */
void     call_begin(struct call *c, handle_t ch);
void     call_end(struct call *c);
/* In a task: wait until c is answered (OK: c->rep, c->m), its channel goes
 * (ERR_PEER_CLOSED) or deadline (ERR_TIMED_OUT); c is not waited for
 * after. Outside a task (nobody to give the CPU to): ERR_BAD_STATE. */
status_t call_wait(struct call *c, uint64_t deadline);
/* An answer m (rep: its bytes) read off ch: true if a task waits for it
 * (then it is the task's), false if not (the reader drops it). */
bool     call_offer(handle_t ch, const void *rep, struct idl_msg *m);
/* ch goes: every call waiting on it ends with ERR_PEER_CLOSED. */
void     call_fail(handle_t ch);
/* Is the caller one of the tasks? */
bool     in_task(void);

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
/* In a task: b's running service stops in order (fsctl.stop: its files
 * closed, everything flushed, the volume clean), killed if it doesn't
 * answer within SYNC_WAIT; its process is gone after. OK, or why it was
 * killed; ERR_PEER_CLOSED: b was retired meanwhile (touch nothing of it). */
status_t fs_stop_in_order(struct vol *b);
/* Close b's client end, and our own channel's two ends (and stop watching
 * for answers). */
void     fs_close_client(struct vol *b);
/* Where volumes asks b's service things itself (stat probes, syncs) and
 * reads the answers: our own channel, or the shared one if there is none. */
handle_t fs_asker(const struct vol *b);
/* What b's instance answers by itself (fs.stat's probe, disk.c), and what
 * lands on the shared channel unread (dropped, fs_answers), come to the
 * port under client_key, if they don't already. */
void     fs_watch_answers(struct vol *b);
/* b's process (start generation `gen`) terminated. */
void     sup_died(struct vol *b, uint32_t gen);
/* b's fsctl channel: an answer came, or it closed (KEY_FSCTL). */
void     fsctl_key(uint64_t key);
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
/* volumes.remount and volesp, before their task: can b (running) be
 * made writable or read-only? OK: already that way; IDL_LATER: it must
 * restart (fs_restart, in a task); ERR_BAD_STATE: its service isn't
 * serving, or something else is under way on it. */
status_t disk_can_restart(const struct vol *b, bool writable);
/* In a task: b's running service stopped in order and started again on
 * a `block` channel opened read-write or read-only (its slot freed if it
 * can't start). ERR_PEER_CLOSED: b went meanwhile. */
status_t fs_restart(struct vol *b, bool writable);
/* volesp's: the boot disk's ESP service, or NULL. */
struct vol *disk_esp(void);
/* volumes.release_disk: does disk id exist? */
bool     disk_exists(uint32_t id);
/* In a task: disk id's mounts stopped in order and gone; the disk isn't
 * mounted again until it is attached again. ERR_NOT_FOUND: no such
 * disk. */
status_t disk_release(uint32_t id);
/* b's handles for its service: FAT_SR_BLOCK, a new `block` channel on its
 * partition, FAT_SR_CTL, and its state and keep channel.
 * ERR_PEER_CLOSED: the disk is gone. */
status_t fs_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* A new `block` channel on b's partition (*out), read-only or not: a call
 * to its disk's driver, waited for in the task (outside one, blocking).
 * ERR_PEER_CLOSED: the disk went, or b was retired meanwhile. */
status_t fs_open_block(struct vol *b, bool read_only, handle_t *out);
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
 * client end was closed); and FAT_SR_OWN, our own channel's, the same way. */
status_t fs_serve_end(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* A new `fs` channel for b (client and serve), its server end marked
 * "receives no handles" (no fs request carries one), so its queue can't
 * hold a channel end and it can always be handed to the next instance
 * (M12's D2); and our own one beside it (b->own, b->own_serve) if there is
 * none. */
status_t fs_new_channel(struct vol *b, handle_t *client, handle_t *serve);
/* Does b keep its `fs` channel while no instance runs? Then it stays a
 * mount meanwhile, and calls on it wait for the next instance. */
bool     fs_channel_kept(const struct vol *b);
/* In a task: every writable mount synced (fs.sync, all at once, each
 * given SYNC_WAIT). The first failure (OK: none). */
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
 * (docs/HISTORY.md, M11.6): per service a keeper and a state VMO, and one
 * warm spare fat for every mount. */
/* `on`: keep a warm spare (no boot word `nospare`). */
void     spare_init(bool on);
/* Every mount gets a pager of `pages` pages' budget (0: the default), made
 * from the root resource init gave (SR_RESOURCE, RIGHT_ROOT_PAGER); without
 * one no mount has a pager. */
void     pager_init(uint64_t pages);
/* Start b: for a `restart` after a death, the warm spare if one waits,
 * else (and for any other start) a new process (fs_spawn); either way
 * with its state and a new keep channel, whose keeper then hands it what
 * was kept. A restart is measured: the log says when it ran and when it
 * first answered. Errors as fs_spawn's. */
status_t fs_run(struct vol *b, bool restart);
/* b's state VMO (made the first time), a new keep channel and b's pager
 * (made the first time) into x (SR_STATE, SR_KEEP, FAT_SR_PAGER), for its
 * next instance. One that can't be made is
 * said and left out: that instance starts fresh. */
void     fs_kept_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n);
/* The `block` channel opened in advance for b (the boot disk's /data)
 * into *out, if there is one and its disk still answers: true. */
bool     fs_block_prepared(const struct vol *b, handle_t *out);
/* b won't run again as it was (retired, given up on, stopped in order):
 * what its keeper kept closes (its clients see ERR_PEER_CLOSED), its state
 * VMO goes, its pager closes (the VMOs it made fail their next access), and
 * a `block` channel opened in advance for it. */
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
