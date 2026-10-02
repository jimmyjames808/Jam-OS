/* utest internals: the suite (main.c: helpers, the kernel's objects and
 * rights, the table of tests; threads.c; drivers.c: drivers as processes
 * and devmgr's edu driver; supervise.c: devmgr's supervision and what a
 * driver's handles can't do; hid.c and hidmouse.c: the hid driver
 * against hidmock.c;
 * disks.c: devmgr's disks and mounts against diskmock.c; logd.c: logd;
 * mix.c: the mixer's arithmetic; mp3.c: <mp3.h>; text.c: <utf8.h>; time.c:
 * <wallclock.h> and <settings.h>; tasks.c: <jam/task.h>; idl.c: the IDL's
 * deferred replies and asynchronous calls; netstack.c and netctl.c:
 * netstack's core over a fake edge and its control channel), the
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
/* hidmouse.c: mice in report protocol, and the report descriptor parser. */
bool t_hid_report_parser(void);
bool t_hid_mouse_report_protocol(void);
bool t_hid_mouse_report_ids(void);
bool t_hid_mouse_boot_kept(void);

/* disks.c: devmgr's disks and mounts against the mock usb-storage
 * (diskmock.c), with the fat service on its partitions. */
bool t_disk_mounts(void);
bool t_disk_not_boot(void);
bool t_disk_other(void);
bool t_disk_fs_restart(void);
bool t_disk_vanishes(void);

/* logd.c: bin/logd with the fat service over a RAM disk as its /data. */
bool t_logd_writes_the_log(void);
bool t_logd_without_data(void);
bool t_logd_data_goes_away(void);
bool t_logd_kernel_log(void);
bool t_logd_openers(void);
bool t_klog_lines(void);

/* audio.c: <audio.h>'s conversions and resampler, <wav.h>'s parser. */
bool t_audio_formats(void);
bool t_audio_resample(void);
bool t_wav_parse(void);

/* mp3.c: <mp3.h>: frame headers, sniffing, decoding through dr_mp3. */
bool t_mp3_header(void);
bool t_mp3_sniff(void);
bool t_mp3_decode(void);

/* text.c: libos's text helpers: <utf8.h>. */
bool t_utf8_well_formed(void);
bool t_utf8_bad_pieces(void);

/* time.c: <wallclock.h> (the calendar, the zones, the clock's calls) and
 * <settings.h> (the parser, and the file on a FAT volume). */
bool t_time_calendar(void);
bool t_time_zones_switch(void);
bool t_time_zones_local(void);
bool t_time_wallclock_calls(void);
bool t_settings_parse(void);
bool t_settings_edit(void);
bool t_settings_file(void);

/* tasks.c: libos's cooperative tasks (<jam/task.h>). */
bool t_tasks_yield_and_wait(void);
bool t_tasks_start_slots_cap(void);

/* idl.c: answering later and calls that don't wait (tools/genidl.py), on
 * the test protocol idltest. */
bool t_idl_answer_later(void);
bool t_idl_later_blocking_clients(void);
bool t_idl_later_handles(void);
bool t_idl_async_through_port(void);

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
bool t_lock_take(void);
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
bool t_devmgr_query_refuses_hda(void);
bool t_devmgr_device_channel(void);
bool t_devmgr_openers(void);
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
bool t_fat_view_limits(void);

/* rights.c: the root's powers, vmo_make_exec. */
bool t_root_powers(void);
bool t_vmo_make_exec(void);
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
bool t_fat_dir_linear(void);
bool t_fat_dir_cursors(void);
bool t_fat_cache(void);

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
bool t_rtl8125_tx_gate(void);
bool t_rtl8125_args(void);
bool t_rtl8125_arp(void);
bool t_rtl8125_stays_off(void);

/* netstack.c: netstack's core and lwIP over a fake edge, in-process;
 * netctl.c: its control channel, in-process and as bin/netstack. */
bool t_netstack_arp(void);
bool t_netstack_ping(void);
bool t_netstack_udp_unreachable(void);
bool t_netstack_malformed(void);
bool t_netstack_fuzz(void);
bool t_netstack_cleared(void);
bool t_netctl_set_and_clear(void);
bool t_netctl_process(void);
bool t_ipv4_text(void);
/* netdrv.c: bin/netstack over a fake driver's netdev rings (netpkt.c's
 * frames, as netstack.c's). */
bool t_netdrv_ping_and_link(void);
bool t_netdrv_restart(void);
bool t_netdrv_hostile_driver(void);

/* netdev.c: the netdev rings' code (<jam/netdev.h>): counts, a hostile
 * peer, the wake flags, the VLAN word, a fake driver thread's exchange. */
bool t_netdev_ring_counts(void);
bool t_netdev_ring_one_thread(void);
bool t_netdev_ring_exchange(void);
bool t_netdev_vlan_word(void);

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

/* update.c: the update manifest's and protocol's parsers (<update.h>,
 * <updwire.h>); updfetch.c: the fetcher's window against a fake server
 * (<updfetch.h>); netlog.c: netlog's datagrams and sender (<netlog.h>). */
bool t_update_manifest(void);
bool t_update_manifest_refusals(void);
bool t_update_manifest_damage(void);
bool t_updwire_golden(void);
bool t_updwire_hostile(void);
bool t_updfetch_clean(void);
bool t_updfetch_lossy(void);
bool t_updfetch_snapshot_gone(void);
bool t_updfetch_failures(void);
bool t_netlog_golden(void);
bool t_netlog_hostile(void);
bool t_netlog_whole_log(void);
bool t_netlog_mac_away(void);
bool t_netlog_ring_dropped(void);
bool t_netlog_forged_acks(void);
bool t_netlog_crash_stream(void);
bool t_netlog_klog_source(void);

/* mix.c: the mixer's arithmetic (<mixmath.h>). */
bool t_mix_gains(void);
bool t_mix_unity_is_exact(void);
bool t_mix_volume_and_master(void);
bool t_mix_dither(void);
bool t_mix_limits(void);
