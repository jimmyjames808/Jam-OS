/* utest internals: the suite (main.c: helpers, the kernel's objects and
 * rights, the table of tests; threads.c; drivers.c: drivers as processes
 * and devmgr's edu driver; supervise.c: devmgr's supervision and what a
 * driver's handles can't do; hid.c and hidmouse.c: the hid driver
 * against hidmock.c;
 * disks.c: devmgr's disks and mounts against diskmock.c; logd.c: logd;
 * mix.c: the mixer's arithmetic; mp3.c: <mp3.h>; text.c: <utf8.h>; time.c:
 * <wallclock.h> and <settings.h>; random.c: random_get and os_random;
 * tasks.c: <jam/task.h>; idl.c: the IDL's
 * deferred replies and asynchronous calls; netstack.c and netctl.c:
 * netstack's core over a fake edge and its control channel), the
 * child modes it spawns (child.c) and the benchmark modes the kernel's
 * bench entry spawns (bench.c). */
#pragma once

#include <idl/devmgr.h>
#include <openkeys.h>
#include <os.h>

#define CMD_BME         0x04    /* PCI command register: Bus Master Enable */

/* "utest <mode> ...": run one child mode, return its exit code. */
int child_main(int argc, char **argv);
/* "utest bench-<what> ...": one user-side benchmark (see bench.c). */
int bench_child(int argc, char **argv);
/* "utest fuzz-calls|fuzz-chans <seed> <ms>": a hostile child for the
 * kernel's fuzz tests (fuzz.c, kernel/test/test_fuzz.c). */
int fuzz_child(int argc, char **argv);
/* "utest posix-<what>": a child of the POSIX hand-over's tests (posixfds.c). */
int posixfds_child(int argc, char **argv);
/* posixfds.c, posixfds_bad.c: the POSIX hand-over (<posixfds.h>) and
 * spawn's stack_size. */
bool t_posixfds_round_trip(void);
bool t_posixfds_many_handles(void);
bool t_posixfds_spawn_child(void);
bool t_spawn_stack_size(void);
bool t_posixfds_malformed(void);
bool t_posixfds_dup(void);
bool t_posixfds_fuzz(void);
bool t_posixfds_writer_refuses(void);
bool t_wants_posix(void);

/* hid.c: the HID driver process against a mock usb-bus and console. */
bool t_hid_typing(void);
bool t_hid_modifiers(void);
bool t_hid_rollover(void);
bool t_hid_repeat(void);
bool t_hid_mouse(void);
bool t_hid_composite(void);
bool t_hid_unplug_and_console_gone(void);
/* hidmouse.c: mice in report protocol, and the report descriptor parser. */
bool t_hid_report_parser(void);
bool t_hid_mouse_report_protocol(void);
bool t_hid_mouse_report_ids(void);
bool t_hid_mouse_boot_kept(void);

/* keymap.c: <keymap.h>'s tables, and the US one against the hid driver. */
bool t_keymap_hid_codes(void);
bool t_keymap_us_keys(void);
bool t_keymap_typing(void);
bool t_keymap_xkb_text(void);
bool t_keymap_matches_hid(void);

/* smoothfont.c: libfun's smooth text (<fun.h>: font.c, fontdraw.c). */
bool t_font_open(void);
bool t_font_measure(void);
bool t_font_pixels(void);
bool t_font_blend(void);
bool t_font_clip(void);
bool t_font_ellipsis(void);
bool t_font_threads(void);
/* "utest font-write": writes into a font, which must kill it. */
int  font_write_child(void);

/* nvme.c: drv/nvme's storage and block protocols on an NVMe drive (skipped
 * without one). */
bool t_nvme_storage(void);
bool t_nvme_block(void);
bool t_nvme_inflight(void);
bool t_nvme_fence(void);
bool t_nvmew_refuse(void);
bool t_nvmew_prepare(void);
bool t_nvmew_check(void);
bool t_nvmew_write(void);
bool t_nvmero_refuse(void);

/* disks.c: devmgr's disks and mounts against the mock usb-storage
 * (diskmock.c), with the fat service on its partitions. */
bool t_disk_mounts(void);
bool t_disk_not_boot(void);
bool t_disk_other(void);
bool t_disk_ext(void);            /* an ext volume on a test disk: bin/extfs, /usbN-test */
bool t_disk_ext_none(void);       /* a Linux-typed partition with no ext volume: left alone */
bool t_disk_stick_ext(void);      /* an ESP with the kernel, then ext data (type 83): the boot disk */
bool t_disk_fs_restart(void);
bool t_disk_own_channel(void);
bool t_disk_vanishes(void);
bool t_disk_answer_storm(void);
bool t_disk_open_ro(void);          /* voldisks.open_partition: a read-only block channel */
bool t_disk_list_channel(void);   /* volumes.list_channel: voldisks alone */

/* logd.c: bin/logd with the fat service over a RAM disk as its /data. */
bool t_logd_writes_the_log(void);
bool t_logd_without_data(void);
bool t_logd_data_goes_away(void);
bool t_logd_kernel_log(void);
bool t_logd_openers(void);
bool t_logd_crash_result(void);
/* chanwire.c: the wire of init's channels with the splash and with logd (P6b). */
bool t_splashch_wire(void);
bool t_crashres_wire(void);
bool t_klog_lines(void);
/* chanpid.c: channel_read's CHANNEL_READ_PID, the writer's pid of each message. */
bool t_chan_writer_pid(void);
/* port_order.c: a channel's close is on its port before a request written
 * after it on another channel (file watching's close-first rule). */
bool t_port_order(void);
/* fswatch.c: file watching's library (<fswatch.h>) in a fake service. */
bool t_fswatch_protocol(void);
bool t_fswatch_bell(void);
bool t_fswatch_queue(void);
bool t_fswatch_ids(void);
bool t_fswatch_caps(void);
bool t_fswatch_rules(void);
bool t_fswatch_adopt(void);
bool t_fswatch_order(void);
/* posixexec.c: a POSIX program that execs in place, seen from a native starter. */
bool t_exec_native(void);

/* audio.c: <audio.h>'s conversions and resampler, <wav.h>'s parser. */
bool t_audio_formats(void);
bool t_audio_resample(void);
bool t_wav_parse(void);

/* audio_out.c: <audio.h>'s float input, room, position, start and pause. The
 * ones with a stream need the mixer and a sound card (else they skip);
 * tools/audio-api-test.sh checks what they play. */
bool t_audio_out_f32(void);
bool t_audio_out_args(void);
bool t_audio_out_slots(void);
bool t_audio_out_s16(void);
bool t_audio_out_f32_play(void);
bool t_audio_out_mono44(void);
bool t_audio_out_room(void);
bool t_audio_out_arm(void);
bool t_audio_out_paced(void);
bool t_audio_out_two(void);
bool t_audio_out_pause(void);

/* mp3.c: <mp3.h>: frame headers, sniffing, decoding through dr_mp3. */
bool t_mp3_header(void);
bool t_mp3_sniff(void);
bool t_mp3_decode(void);

/* text.c: libos's text helpers: <utf8.h>. */
bool t_utf8_well_formed(void);
bool t_utf8_bad_pieces(void);
/* crashinfo.c: a saved panic read back: <crashinfo.h>. */
bool t_crashinfo_report(void);
bool t_crashinfo_code(void);
bool t_crashinfo_hostile(void);

/* time.c: <wallclock.h> (the calendar, the zones, the clock's calls) and
 * <settings.h> (the parser, and the file on a FAT volume). */
bool t_time_calendar(void);
bool t_time_zones_switch(void);
bool t_time_zones_local(void);
bool t_time_wallclock_calls(void);
bool t_time_clock_page(void);
bool t_settings_parse(void);
bool t_settings_edit(void);
bool t_settings_file(void);

/* random.c: the random_get system call and libos's os_random. */
bool t_random_get(void);
bool t_os_random(void);

/* tasks.c: libos's cooperative tasks (<jam/task.h>). */
bool t_tasks_yield_and_wait(void);
bool t_tasks_start_slots_cap(void);

/* keep.c: the keep channel (<keep.h>), a fake service and successor
 * against libos's keeper. */
bool t_keep_put_drop_restore(void);
bool t_keep_restore_batches(void);
bool t_keep_unknown_slots(void);
bool t_keep_refusals(void);
bool t_keep_room(void);
bool t_keep_drain_bounded(void);
bool t_keep_restore_refusals(void);

/* svcstate.c: the state VMO (<svcstate.h>), warm spares, and the
 * "utest svcstate-..." child modes (a spare, a promoted spare, and the
 * reader of the kernel's chanread_* test). */
bool t_svcstate_fresh_and_adopted(void);
bool t_svcstate_slots(void);
bool t_svcstate_answer_mark(void);
bool t_svcstate_refused(void);
bool t_svcstate_standby(void);
int  svcstate_child(int argc, char **argv);

/* idl.c: answering later and calls that don't wait (tools/genidl.py), on
 * the test protocol idltest. */
bool t_idl_answer_later(void);
bool t_idl_later_blocking_clients(void);
bool t_idl_later_handles(void);
bool t_idl_async_through_port(void);
bool t_idl_txid_next(void);
/* idlserve.c: the generated server loop (on channel_reply_wait) and the
 * generated clients' timeouts (_within), on the test protocol null. */
bool t_idl_serve_reply_wait(void);
bool t_idl_serve_gone_client(void);
bool t_idl_within_times_out(void);
/* idlunread.c: the unread bound of the generated servers, and its child
 * ("utest unread-serve|unread-loop": the server, in a job of its own). */
bool t_idl_unread_bound(void);
int unread_child(int argc, char **argv);
/* idlslot.c: requests read into slots (<proto>_take_slot, _run_slot), a
 * reply that waits for the next system call (struct idl_reply) and the
 * `idempotent` keyword, on the test protocol idltest. */
bool t_idl_slot_take_run(void);
bool t_idl_slot_idempotent(void);
bool t_idl_slot_reply_after(void);
/* idlproto.c: several protocols in one .idl file (idltest, idltest_two). */
bool t_idl_wrong_protocol(void);
bool t_idl_big_messages(void);
bool t_idl_consts(void);
/* idlarray.c: arrays of varying length (u8[<=N], str[<=N]) on idltest. */
bool t_idl_var_arrays(void);
bool t_idl_var_refused(void);
bool t_idl_scalar_arrays(void);
/* idlevent.c: events both ways, with arrays and handles (idltest_ev). */
bool t_idl_events_to_server(void);
bool t_idl_events_to_client(void);
/* idlhandle.c: handle arguments, lists and typed handles in requests, replies
 * and events (idltest_h, idltest_ev), and blocking beside asynchronous calls
 * on one end. */
bool t_idl_handle_args(void);
bool t_idl_handle_args_fail(void);
bool t_idl_handle_lists(void);
bool t_idl_handle_lists_async(void);
bool t_idl_handle_server_refuses(void);
bool t_idl_handle_slot(void);
bool t_idl_handle_later(void);
bool t_idl_handle_results_refused(void);
bool t_idl_handle_typed(void);
bool t_idl_events_handle_lists(void);
bool t_idl_txid_mixing(void);

/* main.c: the test running, and helpers the test files share. */
extern const char *utest_cur;
/* "utest only <prefixes>"'s prefixes (NULL: every test runs, or `utest
 * except`'s): the tests that write drives run only when named (nvmewrite.c). */
extern const char *utest_only;
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
/* Does the kernel save AVX state (and the CPU have it)? */
bool avx_usable(void);
/* Load xmm0-15 (ymm0-15 for avx_round) from pat, spin, store them to got
 * (256 or 512 bytes), all in one asm block. */
void sse_round(const uint8_t *pat, uint8_t *got, uint64_t spins);
void avx_round(const uint8_t *pat, uint8_t *got, uint64_t spins);
bool t_fpu_state_survives_preemption(void);

/* fpucall.c: the system call rule for the FPU (ARCHITECTURE.md, "The
 * system call ABI"). */
bool t_fpu_call_keeps_control_words(void);
bool t_fpu_ring3_switch_keeps_all(void);

/* calltimeout.c: channel_call with CHANNEL_CALL_TIMEOUT. */
bool t_call_timeout_relative(void);
/* replywait.c: channel_reply_wait, and its child modes ("utest rw-..."). */
bool t_reply_wait_mark_before_wait(void);
bool t_reply_wait_gone_caller(void);
bool t_reply_wait_second_process(void);
bool t_reply_wait_port(void);
bool t_reply_wait_refusals(void);
/* portagain.c: port_again, the next turn of a reader that keeps to a budget. */
bool t_port_again(void);
bool t_handles_always_moved(void);
/* vmarcount.c: process_get_info's mapping count, and merging as seen from user space. */
bool t_vmar_mappings(void);
bool t_channel_no_handles(void);
bool t_unread_charged_to_writer(void);
int  replywait_child(int argc, char **argv);
/* keptvmo.c: VMO_KEEP_PAGES and VMAR_KEPT_ONLY, and their child modes
 * ("utest kept-..."). */
bool t_kept_vmo_refusals(void);
bool t_kept_vmo_client_and_compositor(void);
int  kept_child(int argc, char **argv);
/* vmochild.c: copy-on-write child VMOs, and the child mode "utest cow-big". */
bool t_vmo_child_shares(void);
bool t_vmo_child_refusals(void);
int  vmochild_child(int argc, char **argv);
/* spawnverified.c: libos's spawn of a verified program, with utest as its pager, and a
 * copy from a mapping whose pager is gone. */
bool t_spawn_verified(void);
bool t_copy_from_lost_page(void);
bool t_many_threads(void);
bool t_lock_take(void);
bool t_thread_fs_base(void);
bool t_futex_calls(void);
/* posixk.c: what the kernel gives POSIX (ids, the tid word,
 * thread_interrupt, CPU time, sys_public_info); "utest tidword". */
bool t_posix_ids(void);
bool t_thread_interrupt_calls(void);
bool t_cpu_ns(void);
bool t_sys_public_info(void);
bool t_process_maps(void);
bool t_vmar_replace_calls(void);
int  tidword_child(void);
bool t_kill_spinning_and_unstarted(void);
bool t_job_kill_reaps_orphans(void);

/* drivers.c */
/* devmgr's control channel, or 0 (with a line saying the test is skipped). */
handle_t devmgr(void);
/* devmgr's supervision view of the device (kind, a, b, n) (devmgr.supervision's
 * results). */
struct dm_sup {
    uint32_t state, restarts, backoff_ms, held, changed;
};
bool supervision(handle_t dm, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                 struct dm_sup *s);
bool t_driver_processes(void);
bool t_driver_killed(void);
bool t_startup_message(void);
bool t_edu_process(void);
bool t_devmgr_query_channel(void);
bool t_devmgr_query_refuses_hda(void);
bool t_devmgr_device_channel(void);
bool t_devmgr_openers(void);
bool t_devmgr_ctl_openers(void);
bool t_devmgr_addresses(void);
bool t_devmgr_kill_process(void);
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
bool t_ns_long_names(void);
bool t_spawn_from_vmo(void);
/* bin/ramfs, started and mounted at point (and the reverse: it must end
 * clean). */
struct ram {
    handle_t job, proc;   /* bin/ramfs */
};
bool ram_start(const char *point, struct ram *r);
bool ram_stop(const char *point, struct ram *r);
/* music.c: the music player's folder walk, play order and bands
 * (user/services/music/tracks.c, spectrum.c). */
bool t_music_scan(void);
bool t_music_order(void);
bool t_music_spectrum(void);
bool t_music_stereo(void);
/* nschild.c: "utest ns-...", "utest fscat <path>", "utest fs-hold <path>"
 * and "utest fat-shell", the namespace tests' children. */
int ns_child(int argc, char **argv);

/* svc.c: services and grants; views.c: views of a filesystem. */
bool t_svc_publish_and_open(void);
bool t_svc_connect(void);
bool t_svc_child_gets_its_grants(void);
bool t_svc_child_gets_views(void);
int  ns_svc_child(const char *name);   /* "utest ns-svc <name>|-" */
int  ns_view_child(const char *how);   /* "utest ns-view r|w" */
bool t_view_etc_names(void);
bool t_fat_views(void);
/* fs_attr.c: the calls M13.6 added to fs and file, as fat and the boot image answer. */
bool t_dirent_types(void);
bool t_fat_attrs(void);
bool t_fat_attrs_read_only(void);
bool t_fat_attr_views(void);
bool t_fs_attr_ns(void);
/* ... and fs.opendir, rename_at, link_at: ERR_NOT_SUPPORTED from fat, its
 * views, the boot image and ramfs. */
bool t_fs_no_dirs(void);
/* extfs.c, extfs_restart.c: the extfs service (bin/extfs) over a RAM disk. */
bool t_extfs_basic(void);
bool t_extfs_links(void);
bool t_extfs_perms(void);
bool t_extfs_writers(void);
bool t_extfs_views(void);
bool t_extfs_error(void);
bool t_extfs_flush(void);   /* a disk with a cache: block.sync where the journal needs it */
bool t_extfs_timeout(void);         /* block calls that time out are tried again */
bool t_extfs_timeout_spent(void);   /* ... and past their tries go read-only as before */
bool t_extfs_late(void);            /* a read held past the wait at depth 32 ends extfs */
bool t_extfs_late_in_order(void);   /* ... at depth 1 it is tried again, or ends it */
bool t_extfs_many(void);    /* 200 files open and 100 views at once */
bool t_extfs_restart_steps(void);
bool t_extfs_restart_handles(void);
bool t_extfs_restart_bad(void);
bool t_extfs_restart_twice(void);   /* in progress at two deaths: run again, not answered */
bool t_extfs_spare(void);
bool t_extfs_restart_pager(void);
/* extfs_map.c: extfs as a pager (U10): mapped files kept right while they
 * change, programs' files guarded, unlinked mapped files kept. */
bool t_extfs_map(void);
/* extfs_wmap.c: writable shared mappings on extfs (WRITEMAP's WE1): one
 * VMO per file and one cache with read, write and truncate; the second
 * process's mode is wmap-store. */
bool t_extfs_wmap(void);
int  wmap_child(int argc, char **argv);
/* extfs_wback.c: mapped files' stores written back (WRITEMAP's WE2): by
 * msync, fsync, fs.sync, age and an orderly stop, over holes and unwritten
 * extents, a full volume's error (once to each mapper and open), a fresh
 * start's keys; "utest wback-map" is its other mapping process. */
bool t_extfs_wback(void);
int  wback_child(int argc, char **argv);
/* extfs_restart.c: a write-back ended at each step of WE2's 2.6. */
bool t_extfs_restart_wback(void);
/* extfs_wpress.c: an extfs that never answers a write-back (the hang=wback
 * power): the writer to its mapping is killed at the pager's deadline, a
 * reader of another volume and utest go on (WRITEMAP's WT1). */
bool t_extfs_wpress(void);
/* extfs_posix.c: posixtest's links section on an extfs volume (libc, U6). */
bool t_extfs_posix(void);
/* extfs_watch.c: file watching on extfs (INOTIFY-PLAN's IE1 and IE2):
 * 2.2's rows, moves, views and a second process, the close rule, the
 * queue's bound and the caps; what a view hides, a watch moved out of its
 * view's reach, watches across restarts; the second process's modes are
 * xwatch-*. */
bool t_extfs_watch_rows(void);
bool t_extfs_watch_moves(void);
bool t_extfs_watch_views(void);
bool t_extfs_watch_order(void);
bool t_extfs_watch_limits(void);
bool t_extfs_watch_hidden(void);
bool t_extfs_watch_reach(void);
bool t_extfs_restart_watch(void);
int  extfs_watch_child(int argc, char **argv);
bool t_view_keys_names(void);
bool t_fat_view_keys(void);
bool t_view_pkg_names(void);
bool t_fat_view_pkg(void);
bool t_view_hides_names(void);
bool t_fat_view_no_etc(void);
bool t_fat_view_limits(void);

/* jit.c: JIT memory (vmo_make_jit): code written, run, patched, passed on;
 * a program started from it never writes its own code (its child's mode
 * "jit-text"). */
bool t_jit(void);
bool t_jit_spawn(void);
int  jit_child(int argc, char **argv);
/* fork.c: libjam's fork (jam_fork) and vmar_clone's arguments, each in a
 * mode of ours ("utest fork-<what>") in a job of its own. */
bool t_fork_memory(void);
bool t_fork_threads(void);
bool t_fork_handles(void);
bool t_fork_nomem(void);
bool t_fork_jit(void);
bool t_fork_vmar_clone_args(void);
int  fork_child_mode(int argc, char **argv);
/* rights.c: the root's powers, vmo_make_exec and _verified. */
bool t_root_powers(void);
bool t_vmo_make_exec(void);
bool t_vmo_make_exec_verified(void);
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

/* heap.c: libos's heap in a long-running program. */
bool t_heap_reuses_freed_space(void);
bool t_heap_large_alloc(void);

/* nsnotice.c: namespace changes a running program never reads. */
bool t_ns_changes_stay_bounded(void);

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
bool t_fat_names_shown(void);
bool t_fat_full_disk(void);
bool t_fat_read_only(void);
bool t_fat_not_formatted(void);
bool t_fat_format_off(void);
bool t_fat_dirty_volume(void);
bool t_fat_disk_gone(void);
bool t_fat_gone_mounting(void);
bool t_fat_timeout(void);         /* block calls that time out tried again (blockcall) */
bool t_fat_timeout_spent(void);   /* ... and past their tries failed as before */
bool t_fat_late(void);            /* a read held past the wait at depth 32 ends fat */
bool t_fat_dir_linear(void);
bool t_fat_dir_cursors(void);
bool t_fat_cache(void);
bool t_fat_gather(void);
/* fat_trunc.c: a truncate to 0 that goes out in steps keeps its chain's head
 * until the end (the disk watched write by write). */
bool t_fat_truncate_steps(void);
/* fat_ctl.c: every fsctl request queued together is answered. */
bool t_fat_ctl_queued(void);
/* fat_restart.c: fat ended at each step of a request and carrying on from
 * its state (the test plays devmgr: state VMO, keeper, restarts). */
bool t_fat_restart_steps(void);
bool t_fat_restart_handles(void);
bool t_fat_restart_bad_request(void);
/* ... and a mapped file's pages through it all (fat with a pager). */
bool t_fat_restart_pager(void);
/* fat_coherent.c: a mapped file's pages follow every write, truncate,
 * rename and removal of its file; a file mapped to run is never changed. */
bool t_fat_map_coherent(void);
/* fatlayout.c: FatFs's structs, as fat's state relies on them. */
bool t_fat_layout(void);

/* netframe.c: <jam/netframe.h> over hand-made and hostile frames, and the
 * RTL8125 driver's pure parts (guard, gate, arguments, ARP). */
bool t_netframe_classify(void);
bool t_netframe_short_frames(void);
bool t_netframe_tag(void);
bool t_netframe_tag_refuses_tagged(void);
bool t_netframe_tx_check(void);
bool t_netframe_tag_copy_is_the_frame(void);
bool t_netframe_rx(void);
bool t_rtl8125_write_guard(void);
bool t_rtl8125_txq_guard(void);
bool t_rtl8125_tx_gate(void);
bool t_rtl8125_args(void);
bool t_rtl8125_arp(void);
bool t_rtl8125_stays_off(void);
bool t_rtl8125_txdesc(void);
bool t_rtl8125_kick(void);
bool t_rtl8125_tx_verdict(void);

/* netplain.c: the untagged network mode of <jam/netframe.h>, the
 * functions that pick by the mode, and <jam/netdev.h>'s mode words. */
bool t_netframe_plain(void);
bool t_netframe_plain_refuses_tagged(void);
bool t_netframe_tx_check_plain(void);
bool t_netframe_tx_modes(void);
bool t_netframe_rx_plain(void);
bool t_netdev_mode_words(void);

/* rtlguard.c: drivers/rtl8125/guard.h, and guard.c (with regs.c, chip.c,
 * tx.c) over a fake chip. */
bool t_rtl8125_guard(void);
bool t_rtl8125_dump(void);
bool t_rtl8125_guard_fake(void);
bool t_rtl8125_stats_nowait(void);

/* rtlrx.c: drivers/rtl8125/rxdesc.h. */
bool t_rtl8125_rxdesc(void);
bool t_rtl8125_rxdesc_laps(void);

/* netsrv.c: the network drivers' netdev server (drivers/lib/netserver.c,
 * linked in) over a fake card, the test as netstack. */
bool t_netserver_session(void);
bool t_netserver_tx(void);
bool t_netserver_rx(void);

/* netstack.c: netstack's core and lwIP over a fake edge, in-process;
 * netctl.c: its control channel, in-process and as bin/netstack. */
bool t_netstack_arp(void);
bool t_netstack_ping(void);
bool t_netstack_udp_unreachable(void);
bool t_netstack_malformed(void);
bool t_netstack_fuzz(void);
bool t_netstack_cleared(void);
/* nettcp.c and tcpabuse.c: netstack's TCP (stack.c's edge, tcp.c's
 * connections) in-process, the test as the peer and the program. */
bool t_nettcp_connect(void);
bool t_nettcp_listen(void);
bool t_nettcp_slow_reader(void);
bool t_nettcp_reset(void);
bool t_nettcp_syn_flood(void);
bool t_nettcp_pool_full(void);
bool t_nettcp_malformed(void);
bool t_nettcp_fuzz(void);
bool t_nettcp_hostile_ring(void);
bool t_nettcp_card_full(void);
/* nettcpscale.c: window scaling and lwIP's memory shared out. */
bool t_nettcp_window_scale(void);
bool t_nettcp_listen_scale(void);
bool t_nettcp_heap_shares(void);
/* nettcpooseq.c: segments past a hole kept (bounded), SACK. */
bool t_nettcp_ooseq_sack(void);
bool t_nettcp_ooseq_bounds(void);
/* nettcpopts.c: a connection's options (Nagle, keep-alive), in-process. */
bool t_nettcp_nodelay(void);
bool t_nettcp_keepalive(void);
bool t_nettcp_reuse(void);
/* nettcpsock.c: programs' TCP calls on bin/netstack over the fake driver. */
bool t_nettcp_limits(void);
bool t_nettcp_bulk_rings(void);
bool t_nettcp_options(void);
bool t_nettcp_reuse_api(void);
bool t_netctl_set_and_clear(void);
bool t_netctl_read_only(void);
bool t_netctl_process(void);
bool t_ipv4_text(void);
/* netdrv.c: bin/netstack over a fake driver's netdev rings (netpkt.c's
 * frames, as netstack.c's). */
bool t_netdrv_ping_and_link(void);
bool t_netdrv_link_flap(void);
bool t_netdrv_restart(void);
bool t_netdrv_hostile_driver(void);
/* netsock.c: programs' sockets and pings on /svc/net, through bin/netstack
 * over netdrv.c's fake driver; netabuse.c: its limits, hostile programs,
 * a slow reader, netctl's DHCP socket. */
bool t_netsock_udp(void);
bool t_netsock_ping(void);
bool t_netsock_iface(void);
bool t_netsock_limits(void);
bool t_netsock_caps(void);
bool t_netsock_mem(void);
/* netloop.c: loopback (127.0.0.0/8) through netstack over the fake driver:
 * UDP, ping, TCP, and frames from the network with a loopback address. */
bool t_netloop_udp(void);
bool t_netloop_ping(void);
bool t_netloop_tcp(void);
bool t_netloop_martian(void);
bool t_netsock_hostile(void);
bool t_netsock_slow_reader(void);
bool t_netsock_busy_client(void);
bool t_netsock_len_lies(void);
bool t_netsock_short_counts(void);
bool t_netsock_dhcp(void);
/* netrings.c: sockets' rings against netstack: the fair shares, a hostile
 * program's rings, real UDP sockets in a wait set; and libos against a
 * netstack that lies (netsock_len_lies, netsock_short_counts). */
bool t_netsock_shares(void);
bool t_netsock_hostile_rings(void);
bool t_netwait_udp(void);
/* netbench.c: datagrams through a socket and netstack, timed (BENCH.md). */
bool t_netsock_bench(void);
/* netlisten.c: the listen permission (netstack's listen.h, <wants.h>'s
 * `svc net listen`); sntp.c: bin/sntp's request and checks (ntp.c). */
bool t_netlisten_udp(void);
bool t_netlisten_low(void);
bool t_netlisten_wants(void);
bool t_sntp_request_and_reply(void);
bool t_sntp_checks(void);
bool t_sntp_times(void);
bool t_sntp_fuzz(void);
/* http.c: <http.h>, the HTTP of fetch and serve: URLs, response and
 * request heads, chunked bodies, ranges, and hostile heads. */
bool t_http_url(void);
bool t_http_response(void);
bool t_http_chunks(void);
bool t_http_request(void);
bool t_http_fuzz(void);

/* jwl.c, jwl_bad.c, jwl_batch.c, jwl_conn.c and jwl_fuzz.c (jwltest.h):
 * libjwl's codec, object map and transport (<jwl.h>) against stand-in
 * tables: round trips, every malformed message and batch, the window, and
 * random and mutated batches. */
bool t_jwl_tables(void);
bool t_jwl_tables_generated(void);
bool t_jwl_roundtrip(void);
bool t_jwl_roundtrip_events(void);
bool t_jwl_encode_limits(void);
bool t_jwl_map_client_ids(void);
bool t_jwl_map_server_ids(void);
bool t_jwl_bad_framing(void);
bool t_jwl_bad_strings(void);
bool t_jwl_bad_objects(void);
bool t_jwl_bad_bind(void);
bool t_jwl_bad_events(void);
bool t_jwl_bad_batches(void);
bool t_jwl_bad_compositor(void);
bool t_jwl_bad_empty_run(void);
bool t_jwl_conn_messages(void);
bool t_jwl_conn_batches(void);
bool t_jwl_conn_window(void);
bool t_jwl_conn_never_reads(void);
bool t_jwl_conn_send_refusals(void);
bool t_jwl_conn_new_ids(void);
bool t_jwl_conn_held_handles(void);
bool t_jwl_conn_held_merged(void);
bool t_jwl_conn_handle_queue(void);
bool t_jwl_fuzz_decode(void);
bool t_jwl_fuzz_conn(void);

/* jwlc_window.c, jwlc_seat.c and jwlc_reconnect.c, over the fake
 * compositor of jwlc_fake.c (jwlcfake.h): libjwl's client side
 * (<jwl_client.h>). */
bool t_jwlc_setup(void);
bool t_jwlc_window(void);
bool t_jwlc_window_sizes(void);
bool t_jwlc_held_retry(void);
bool t_jwlc_fuzz_events(void);
bool t_jwlc_fuzz_batches(void);
bool t_jwlc_keyboard(void);
bool t_jwlc_keyboard_hostile(void);
bool t_jwlc_key_burst(void);
bool t_jwlc_pointer(void);
bool t_jwlc_no_keyboard(void);
bool t_comp_layout_wait(void);
bool t_comp_super_enter(void);
/* compplumb.c: the desktop's plumbing (D2b) */
bool t_comp_notify(void);
bool t_comp_notify_splash(void);
bool t_comp_notify_dismissed(void);
bool t_comp_notify_flood(void);
bool t_comp_launch(void);
bool t_comp_volume_net(void);
bool t_comp_no_keyboard(void);
bool t_comp_overlay_late(void);
bool t_comp_early_keys(void);
bool t_comp_screen_on_top(void);
bool t_jwlc_reconnect(void);
bool t_jwlc_connect_pending(void);
bool t_jwlc_protocol_error(void);
bool t_jwlc_slow_reader(void);
bool t_jwlc_blocking(void);
/* funwin.c: libfun's window (gfx_open on a compositor), over the same fake. */
bool t_fun_window_present(void);
bool t_fun_window_present_rects(void);
bool t_fun_window_input(void);
bool t_fun_window_reconnect(void);
bool t_fun_window_resize(void);
/* funscreen.c: libfun without a compositor: no window, no screen borrowed. */
bool t_fun_no_window(void);

/* comp.c, comp_bad.c: bin/compositor headless, driven by Wayland clients
 * over real channels (comptest.h). */
bool t_comp_globals(void);
bool t_comp_surface(void);
bool t_comp_bad_requests(void);
bool t_comp_caps(void);
bool t_comp_connections(void);
bool t_comp_connect_storm(void);
bool t_comp_client_crash(void);
bool t_comp_never_reads(void);
/* Clients lost every way, round after round: nothing stays charged to the compositor. */
bool t_comp_lost_clients(void);
bool t_comp_slow_clients_kept(void);
bool t_comp_fuzz(void);
bool t_comp_chaos(void);
/* The same against the desktop's compositor (/svc/wayland): `utest only comp_live`. */
bool t_comp_live_bad_requests(void);
bool t_comp_live_caps(void);
bool t_comp_live_client_crash(void);
bool t_comp_live_never_reads(void);
bool t_comp_live_fuzz(void);
/* msgbudget.c: a client flooding the compositor waits on its own
 * connection (CHANNEL_BYTE_BOUND), never using up the root's message bytes. */
bool t_msgbudget_comp_flood(void);
bool t_comp_regions(void);
/* conwin.c: the console's window mode: its grid and padding, the view,
 * terminal.font, keys and pointer (wlinput.c, view.c, linked in). */
bool t_conwin_grid(void);
bool t_conwin_padding(void);
bool t_conwin_view(void);
bool t_conwin_font_setting(void);
bool t_conwin_keys(void);
bool t_conwin_pointer(void);
bool t_conwin_pointer_shape(void);
bool t_con_hostile_compositor(void);
/* conconn.c: the console's connection back end (bin/console "conn"). */
/* verdict.c: passes, or fails or crashes on request (UTEST_CANARY): the
 * shell tests' proof that a failing utest fails the script. */
bool t_verdict_canary(void);
/* session.c, sessbad.c: init's sessions (/svc/session; sessrig.h). Only
 * a shell-mode init serves them: elsewhere each says it is skipped. */
bool t_session_shell(void);
bool t_session_command(void);
bool t_session_hangup(void);
bool t_session_limit(void);
bool t_session_posix(void);
bool t_session_bad_args(void);
bool t_session_garbage(void);
bool t_session_ctl_first(void);
bool t_session_unread(void);
bool t_session_opens(void);
bool t_session_no_grant(void);
/* "utest session-probe" (sessbad.c). */
int session_child(int argc, char **argv);
bool t_con_conn_shell(void);
bool t_con_conn_log(void);
bool t_con_conn_hostile(void);
bool t_con_conn_backpressure(void);
bool t_con_conn_stop(void);
/* contty.c: the terminal's POSIX side in the console (open_tty, `tty`). */
bool t_con_tty_canonical(void);
bool t_con_tty_raw(void);
bool t_con_tty_focus(void);
bool t_con_tty_order(void);
bool t_con_tty_unread(void);
/* conpaint.c: the console's cells drawn (cellpaint.c, linked in). */
bool t_conpaint_metrics(void);
bool t_conpaint_cells(void);
bool t_conpaint_bold(void);
bool t_conpaint_blocks(void);
bool t_conpaint_bitmap(void);
/* consel.c: copy and paste in a terminal window, the pure parts (select.c,
 * wlinput.c, cellpaint.c). */
bool t_consel_drag(void);
bool t_consel_word_line(void);
bool t_consel_scrollback(void);
bool t_consel_keys(void);
bool t_consel_tint(void);
/* shpaste.c: the shell's bracketed paste (sh_paste.c). */
bool t_sh_paste(void);
/* shimage.c: a /data program's page table and its list read in pieces
 * (libos's dataprog.c). */
bool t_sh_ptable(void);
bool t_sh_image_wants(void);
/* execsvc.c: init's /svc/exec (exectest and execwide from /data). */
bool t_exec_sealed_runs(void);
bool t_exec_wider_than_parent(void);
bool t_exec_refused(void);
bool t_exec_changed(void);
bool t_exec_handover(void);
bool t_exec_within(void);
bool t_exec_starter(void);
bool t_exec_no_leak(void);
bool t_exec_cleanup(void);
/* unixsvc.c: the named socket broker, /svc/unix (bin/unix). */
bool t_unix_names(void);
bool t_unix_backlog(void);
bool t_unix_limits(void);
bool t_unix_own(void);
/* "utest wl-crash": a Wayland client on the channel at SR_USER that makes
 * a pool, a buffer and a surface, then crashes; "wl-crash-attach" crashes
 * between the attach and the commit. */
int  comp_child(int argc, char **argv);
/* jwlc_real.c: libjwl's client side against the real compositor, headless. */
bool t_jwlc_real_compositor(void);
/* jwlc_restore.c: windows back in their places after a real compositor's restart. */
bool t_jwlc_restore(void);
/* jwlc_clip.c: libjwl's clipboard against the real compositor, headless. */
bool t_jwlc_clip(void);
/* compseat.c, compinput.c: the compositor's seat (input sources, compctl,
 * focus, the keyboard and the pointer; compseat.h), and <termkeys.h>. */
bool t_comp_seat_keymap(void);
bool t_comp_seat_focus(void);
bool t_comp_seat_grab(void);
bool t_comp_seat_reserved(void);
bool t_comp_seat_text(void);
bool t_comp_seat_ctl(void);
bool t_comp_seat_move(void);
/* compdata.c: the compositor's clipboard (wl_data_device_manager). */
bool t_comp_data_selection(void);
bool t_comp_data_rules(void);
bool t_termkeys(void);
/* ldisc.c: the line discipline (<ldisc.h>) and keys into bytes (<keybytes.h>). */
bool t_ldisc_table(void);
bool t_ldisc_queue(void);
bool t_ldisc_read_rules(void);
bool t_keybytes(void);
/* comp_paint.c: the compositor's painting, through its test scene
 * (windows with no client) run headless (testscene.h). */
bool t_comp_paint_overlap(void);
bool t_comp_paint_cull(void);
bool t_comp_paint_damage(void);
bool t_comp_paint_fullscreen(void);
bool t_comp_paint_cursor(void);
bool t_comp_paint_blank(void);
bool t_comp_paint_overlay(void);
bool t_comp_paint_splash(void);
bool t_comp_paint_splash_notice(void);
bool t_comp_paint_overlay_open(void);
/* comp_look.c: the look (look.h) the same way: title bars and circles,
 * rounded corners, shadows, tiling's borders, the wallpaper. */
bool t_comp_look_title(void);
bool t_comp_look_corners(void);
bool t_comp_look_shadow(void);
bool t_comp_look_buttons(void);
bool t_comp_look_tiled(void);
bool t_comp_look_wallpaper(void);

/* compwm.c: the compositor's window manager, linked in and driven by fake
 * toplevels and pointer calls: floating, tiling, the switch, focus, the
 * layout setting. compxdg.c: xdg-shell against bin/compositor headless. */
bool t_wm_floating_place(void);
bool t_wm_floating_move(void);
bool t_wm_floating_resize(void);
bool t_wm_states(void);
bool t_wm_tiling(void);
bool t_wm_switch(void);
bool t_wm_focus(void);
bool t_wm_window_at(void);
bool t_wm_layout_setting(void);
/* compwm2.c, compwm3.c: tiling's dwindle tree, its gaps, the window keys,
 * Super+drag and the glide, on the same harness. */
bool t_wm_tile_tree(void);
bool t_wm_tile_small(void);
bool t_wm_tile_gaps(void);
bool t_wm_tile_push(void);
bool t_wm_tile_float(void);
bool t_wm_tile_mins(void);
/* compgeom.c: the window geometry (set_window_geometry) placing a window,
 * its frame and its hits, on the same harness. */
bool t_wm_geometry(void);
bool t_wm_save_restore(void);
bool t_wm_save_late(void);
bool t_wm_save_full(void);
bool t_wm_save_bad(void);
bool t_wm_save_big_min(void);
bool t_wm_save_unmapped(void);
bool t_wm_focus_dir(void);
bool t_wm_swap(void);
bool t_wm_reflow(void);
bool t_wm_keys(void);
/* compdesk.c, compdesk2.c: the desktop (screens, minimising, the strip,
 * animations, Alt+Tab, the search box, popovers, notifications) linked in
 * on compwm.c's harness (compwm.h). */
bool t_desk_screens(void);
bool t_desk_fullscreen(void);
bool t_desk_minimise(void);
bool t_desk_room(void);
bool t_desk_strip(void);
bool t_desk_anim(void);
bool t_desk_anim_big(void);
bool t_desk_cursors(void);
bool t_desk_alttab(void);
bool t_desk_alttab_gone(void);
bool t_desk_search(void);
bool t_desk_popover(void);
bool t_desk_notify(void);
bool t_desk_notify_held(void);
bool t_desk_notify_dismissed(void);
bool t_desk_overlay(void);
bool t_desk_overlay_first(void);
/* compwmseat.c: the window manager with the real seat: drag, close box,
 * Super+Q, Super+F, Super+T on an xdg toplevel. */
bool t_wm_seat(void);
bool t_wm_seat_cursors(void);
bool t_wm_seat_overlay_keys(void);
bool t_xdg_toplevel(void);
bool t_xdg_tiling(void);
bool t_xdg_tile_resize(void);
bool t_xdg_popup(void);
/* compplace.c: where a positioner puts a popup (xdgplace.c, linked in);
 * comppopup.c: popups shown, followed, repositioned, at the screen's
 * edges, the grab (keys, pointer, nested, dismissal), the errors. */
bool t_popup_place(void);
bool t_popup_show(void);
bool t_popup_grab(void);
bool t_popup_bad(void);
bool t_xdg_unacked(void);
bool t_xdg_errors(void);
/* compver.c: the versions above G1's (wl_compositor 6, wl_shm 2, wl_output
 * 4, wl_seat 9, xdg_wm_base 6), each new event at its version and not
 * below, xdg-decoration's modes (the owner's rule), toggled and refused,
 * and a client-side window placed by its window geometry. */
bool t_comp_versions(void);
bool t_comp_seat_v9(void);
bool t_xdg_versions(void);
bool t_xdg_tiled(void);
bool t_xdg_decoration(void);
bool t_xdg_decoration_bad(void);
bool t_xdg_geometry(void);
/* compsub.c: subsurfaces: trees painted and clipped, the frame round a
 * tree, synchronized and desynchronized commits, the order, input to the
 * topmost surface, the hostile cases and the caps. */
bool t_comp_sub_paint(void);
bool t_comp_sub_frame(void);
bool t_comp_sub_commits(void);
bool t_comp_sub_input(void);
bool t_comp_sub_hostile(void);

/* netdev.c: the netdev rings' code (<jam/netdev.h>): counts, a hostile
 * peer, the wake flags, the VLAN word, a fake driver thread's exchange. */
bool t_netdev_ring_counts(void);
bool t_netdev_ring_one_thread(void);
bool t_netdev_ring_exchange(void);
bool t_netdev_vlan_word(void);

/* sockring.c: a socket's rings (<sockring.h>): counts, datagrams and a
 * byte stream round rings that wrap, the end, the wake flags, a hostile
 * peer, a fake netstack thread's exchange with the rights it hands out. */
bool t_sockring_counts(void);
bool t_sockring_dgram(void);
bool t_sockring_stream(void);
bool t_sockring_wake(void);
bool t_sockring_hostile(void);
bool t_sockring_exchange(void);

/* netwait.c and netwaitrun.c: wait sets (<netwait.h>) over fake sockets
 * (fakesock.c): the calls, timeouts, wakes and costs; a socket's life and
 * plain handles; random traffic checked look by look against the rings;
 * a fake netstack thread against the test blocking in the set; edge
 * entries and netwait_block on a channel. */
bool t_netwait_api(void);
bool t_netwait_states(void);
bool t_netwait_level(void);
bool t_netwait_stress(void);
bool t_netwait_edge(void);

/* dhcp.c and dhcpc.c: the DHCP client's messages and its state machine
 * (user/services/dhcp/msg.c, client.c); dns.c and dnsres.c: the
 * resolver's messages, its queries and its cache (user/services/dns/);
 * netfuzz.c and nettest.h: what they share. */
bool t_dhcp_build(void);
bool t_dhcp_parse_sample(void);
bool t_dhcp_parse_truncated(void);
bool t_dhcp_parse_options(void);
bool t_dhcp_parse_addresses(void);
bool t_dhcp_overload(void);
bool t_dhcp_fuzz(void);
bool t_dhcpc_lease_cycle(void);
bool t_dhcpc_retransmit(void);
bool t_dhcpc_nak(void);
bool t_dhcpc_wrong_replies(void);
bool t_dhcpc_probe(void);
bool t_dhcpc_reboot(void);
bool t_dhcpc_times(void);
bool t_dhcpc_stop(void);
bool t_dhcpc_hostile(void);
bool t_dns_names(void);
bool t_dns_parse_samples(void);
bool t_dns_parse_answers(void);
bool t_dns_names_hostile(void);
bool t_dns_parse_hostile(void);
bool t_dns_fuzz(void);
bool t_dnsres_basic(void);
bool t_dnsres_retries(void);
bool t_dnsres_slow_peer(void);
bool t_dnsres_cname(void);
bool t_dnsres_failures(void);
bool t_dnsres_ports(void);
bool t_dnsres_cache(void);
bool t_dnsres_hostile(void);
bool t_dnsres_shares(void);
/* dnsd.c: bin/dns's sockets (socks.c) against a fake netstack */
bool t_dnsd_sockets(void);
bool t_dnsd_shares(void);

/* update.c: the update manifest's and protocol's parsers (<update.h>,
 * <updwire.h>) and the manifest's signature; updfetch.c: the fetcher's
 * window against a fake server (<updfetch.h>); netlog.c: netlog's
 * datagrams and sender (<netlog.h>). */
bool t_update_manifest(void);
bool t_update_manifest_menu(void);
bool t_update_manifest_abi(void);
bool t_update_manifest_refusals(void);
bool t_update_manifest_damage(void);
bool t_update_build_net(void);
bool t_update_build_abi(void);
bool t_update_signature(void);
/* updoffer.c: the offer call of `update` between update_make_offer and update_answer_reply. */
bool t_update_offer_call(void);
bool t_updwire_golden(void);
bool t_updwire_hostile(void);
bool t_updfetch_clean(void);
bool t_updfetch_lossy(void);
bool t_updfetch_snapshot_gone(void);
bool t_updfetch_failures(void);
bool t_updfetch_menu(void);
/* bootmenu.c: the boot menu's check (<bootmenu.h>). */
bool t_bootmenu_good(void);
bool t_bootmenu_refusals(void);
bool t_bootmenu_files(void);
bool t_bootmenu_fuzz(void);
/* pkgfmt.c: the ports formats (<pkgfmt.h>), over PO3's vectors; the
 * checks themselves are pkgcases.c, pkgfuzz.c, pkgjnl.c, pkgmisc.c (shared
 * with the Mac's tools/pkgcheck.c). */
bool t_pkgfmt_vectors(void);
bool t_pkgfmt_misc(void);
bool t_pkgfmt_table_is_allowd(void);
bool t_pkgfmt_lists_are_allows(void);
bool t_pkgfmt_note_is_wants(void);
bool t_pkgfmt_journal(void);
bool t_pkgfmt_fuzz(void);

/* pkgctl.c: init's approvals of a package's programs (init's pkgapprove.c
 * over a RAM disk's fat at /pkgt), the settle of a journal, the want
 * `mount /data rw pkg`, and init's own pkgctl channel. */
bool t_pkgctl_good(void);
bool t_pkgctl_changed_file(void);
bool t_pkgctl_changed_table(void);
bool t_pkgctl_wrong_key(void);
bool t_pkgctl_refused_list(void);
bool t_pkgctl_other_version(void);
bool t_pkgctl_settle(void);
bool t_pkgctl_want(void);
bool t_pkgctl_init(void);

/* pkgtxn.c, pkgplan.c, pkgcut.c: bin/pkg's transactions, plans and cuts
 * (user/services/pkg), over pkgmk.c's RAM disk and package maker. */
bool t_pkgtxn_install(void);
bool t_pkgtxn_big(void);
bool t_pkgtxn_refused(void);
bool t_pkgtxn_init_no(void);
bool t_pkgtxn_upgrade(void);
bool t_pkgtxn_remove(void);
bool t_pkgtxn_hold(void);
bool t_pkgtxn_busy(void);
bool t_pkgtxn_db(void);
bool t_pkgtxn_runs(void);
bool t_pkgplan_install(void);
bool t_pkgplan_upgrade(void);
bool t_pkgplan_remove(void);
bool t_pkgplan_paths(void);
bool t_pkgcut_install(void);
bool t_pkgcut_upgrade(void);
bool t_pkgcut_remove(void);
bool t_pkgcut_double(void);
bool t_netlog_golden(void);
bool t_netlog_hostile(void);
bool t_netlog_whole_log(void);
bool t_netlog_mac_away(void);
bool t_netlog_ring_dropped(void);
bool t_netlog_forged_acks(void);
bool t_netlog_sender_restarted(void);
bool t_netlog_crash_stream(void);
bool t_netlog_klog_source(void);

/* mix.c: the mixer's arithmetic (<mixmath.h>). */
bool t_mix_gains(void);
bool t_mix_unity_is_exact(void);
bool t_mix_volume_and_master(void);
bool t_mix_dither(void);
bool t_mix_limits(void);

/* handles.c: M12 K3's calls (handle_info, ...). */
bool t_handle_info(void);
bool t_length_limits(void);
bool t_replace_consumes(void);

/* M12 P1b: input as events (hid.c, compseat.c). */
bool t_hid_leds(void);
bool t_hid_leds_storm(void);
bool t_hid_queue_full(void);
bool t_comp_seat_leds(void);
/* fat.c: an open file is known by its directory entry (P5). */
bool t_fat_open_moved(void);
/* volwatch.c: volumes' watches and release_disk (S2a). */
bool t_volumes_watch(void);
bool t_volumes_release_disk(void);
bool t_volumes_new_channel(void);
/* volwatch.c: volumes answers while a remount waits (S2b). */
bool t_volumes_loop_answers(void);
/* fat_hostile.c: fat against clients that misbehave (M12.1). */
bool t_fat_unread_flood(void);
bool t_fat_rename_into_alias(void);
/* idlfuzz.c: every protocol's generated dispatch fed hostile messages (M12.1). */
bool t_idl_fuzz(void);
/* idlfuzz.c: fat and volumes fed the same messages, live (M12.1). */
bool t_fat_hostile(void);
bool t_volumes_hostile(void);
/* initctl.c: init's control channel with names as strings (P6b). */
bool t_initctl_strings(void);
/* idlfuzz.c: devmgr and init fed the same messages, live; svc.c: a shared
 * /svc end one holder never reads (M12.1 part 1b). */
bool t_devmgr_hostile(void);
bool t_initctl_hostile(void);
bool t_svc_shared_unread(void);
