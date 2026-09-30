/* utest internals: the suite (main.c: helpers, the kernel's objects and
 * rights, the table of tests; threads.c; drivers.c: drivers as processes
 * and devmgr's edu driver; supervise.c: devmgr's supervision and what a
 * driver's handles can't do; hid.c: the hid driver against hidmock.c;
 * disks.c: devmgr's disks and mounts against diskmock.c; logd.c: logd), the
 * child modes it spawns (child.c) and the benchmark modes the kernel's
 * bench entry spawns (bench.c). */
#pragma once

#include <devmgr.h>
#include <os.h>

#define CHANNEL_MSG_MAX 65536   /* CHANNEL_MAX_BYTES in the kernel */
#define CMD_BME         0x04    /* PCI command register: Bus Master Enable */

/* "utest <mode> ...": run one child mode, return its exit code. */
int child_main(int argc, char **argv);
/* "utest bench-<what> ...": one user-side benchmark (see bench.c). */
int bench_child(int argc, char **argv);

/* hid.c: the HID driver process against a mock usb-bus and console. */
bool t_hid_typing(void);
bool t_hid_modifiers(void);
bool t_hid_rollover(void);
bool t_hid_repeat(void);
bool t_hid_mouse(void);
bool t_hid_composite(void);
bool t_hid_unplug_and_console_gone(void);

/* disks.c: devmgr's disks and mounts against the mock usb-storage
 * (diskmock.c), with the fat service on its partitions. */
bool t_disk_mounts(void);
bool t_disk_not_boot(void);
bool t_disk_fs_restart(void);
bool t_disk_vanishes(void);

/* logd.c: bin/logd with the fat service over a RAM disk as its /data. */
bool t_logd_writes_the_log(void);
bool t_logd_without_data(void);
bool t_logd_data_goes_away(void);
bool t_logd_kernel_log(void);

/* main.c: the test running, and helpers the test files share. */
extern const char *utest_cur;
handle_t own_job(void);
status_t info_of(handle_t job, struct job_info *ji);
status_t new_job(handle_t *out);
/* Start "utest <mode> [arg]" in job, with up to one extra handle. */
status_t child(const char *mode, const char *arg, handle_t job, handle_t extra, handle_t *proc);
/* Run a child to the end in a fresh job; *info says how it ended, and the
 * job must be empty afterwards. limit_kind/limit (0 = none) go on the job. */
bool run_child(const char *mode, uint32_t limit_kind, uint64_t limit, struct process_info *info);

/* threads.c */
/* Wait for n threads to end, closing their handles. */
bool wait_threads(const handle_t *th, unsigned n);
bool t_fpu_state_survives_preemption(void);
bool t_many_threads(void);
bool t_kill_spinning_and_unstarted(void);
bool t_job_kill_reaps_orphans(void);

/* drivers.c */
/* devmgr's control channel, or 0 (with a line saying the test is skipped). */
handle_t devmgr(void);
status_t dm_call(handle_t dm, uint32_t op, uint16_t vendor, uint16_t device,
                 struct devmgr_rep *r, handle_t *hs, uint32_t *nh);
/* devmgr's supervision view of a device into *r. */
bool supervision(handle_t dm, uint16_t vendor, uint16_t device, struct devmgr_rep *r);
bool t_driver_processes(void);
bool t_driver_killed(void);
bool t_startup_message(void);
bool t_edu_process(void);
bool t_devmgr_query_channel(void);
bool t_edu_killed_mid_dma(void);

/* ns.c: the file namespace, against the bootfs server and bin/ramfs. */
bool t_ns_boot_mount(void);
bool t_ns_boot_read_only(void);
bool t_ns_path_rules(void);
bool t_ns_mount_point_names(void);
bool t_ns_read_write(void);
bool t_ns_server_dies(void);
bool t_ns_child_sees_only_its_mounts(void);
bool t_ns_mounts_reach_a_running_child(void);
bool t_ns_malformed_messages(void);
bool t_spawn_from_vmo(void);
/* nschild.c: "utest ns-...", "utest fscat <path>", "utest fs-hold <path>"
 * and "utest fat-shell", the namespace tests' children. */
int ns_child(int argc, char **argv);
#define NS_HELLO "hello from utest\n"   /* what the children expect in <mount>/hello */
/* text into a new file at path. */
status_t ns_put(const char *path, const char *text);
/* The file at path holds exactly text. */
bool ns_holds(const char *path, const char *text);
/* A utest child (mode, arg) in a fresh job with the mounts `ns` (NULL: no
 * namespace) and one extra handle (extra.h 0: none); *ns_out as spawn's. */
status_t ns_child_start(const char *mode, const char *arg, const char *const *ns,
                        struct spawn_handle extra, handle_t *ns_out, handle_t *proc);
/* The child ended by itself with this code (its handle is closed). */
bool ns_child_exits(handle_t proc, int64_t code);

/* nsfat.c: the namespace over the real fat service on a RAM disk. */
bool t_ns_fat_mount(void);
int fat_shell(void);

/* supervise.c */
bool t_supervised_restart(void);
bool t_supervised_backoff(void);
bool t_supervised_give_up(void);
bool t_driver_handle_limits(void);

/* fat.c, fat_names.c: the fat service process over a RAM-disk `block`
 * server. */
bool t_fat_format(void);
bool t_fat_files(void);
bool t_fat_dirs(void);
bool t_fat_names(void);
bool t_fat_full_disk(void);
bool t_fat_read_only(void);
bool t_fat_not_formatted(void);
bool t_fat_dirty_volume(void);
bool t_fat_disk_gone(void);
