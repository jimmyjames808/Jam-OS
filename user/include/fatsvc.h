/* Starting the fat service (bin/fat, user/services/fat): the startup
 * handles it takes, for whoever spawns it (devmgr; utest's RAM-disk tests).
 *
 *   FAT_SR_BLOCK   a `block` channel (abi/idl/block.idl): the one partition
 *                  this instance may touch. If its info says read-only, fat
 *                  refuses every change (the /esp instance).
 *   FAT_SR_SERVE   the server end of the channel fat serves `fs` on
 *                  (abi/idl/fs.idl): SR_DRIVER(DR_SERVE), as a driver's.
 *                  When its client end closes, fat closes every file, syncs
 *                  and exits 0.
 *   FAT_SR_CTL     optional: the server end of an `fsctl` channel
 *                  (abi/idl/fsctl.idl): its holder can have fat stop in
 *                  order (files closed, volume clean, exit 0).
 *   SR_RESOURCE    optional: the root resource, used only to read the
 *                  real-time clock for file timestamps. Without it every
 *                  timestamp is 2026-01-01 00:00:00.
 *   SR_STATE       optional: its state VMO (<svcstate.h>), FAT_STATE_SIZE
 *                  bytes, made and kept by devmgr across its restarts, with
 *                  SVCSTATE_SERVICE_RIGHTS.
 *   SR_KEEP        optional: its end of the keep channel (<keep.h>); devmgr
 *                  is the keeper, and hands a successor what was kept.
 *                  Not used by fat yet: it makes its own state VMO and
 *                  starts fresh (docs/M11.6-PLAN.md, stage F3).
 *
 * devmgr also starts one fat with nothing but SR_STANDBY, the warm spare
 * (<svcstate.h>), and promotes it with the handles above when a mount's
 * fat dies.
 *
 * argv[1], if given, names the instance in its log lines ("/data").
 *
 * Formatting is off unless argv[2] is FAT_ARG_FORMAT. Only then, and only
 * on a writable channel, is a blank partition (no boot signature in its
 * first sector) formatted, label JAMOS-DATA. devmgr passes the flag for
 * one partition only: the data partition of the disk Jam OS booted from.
 * Without the flag fat writes nothing to a partition it can't mount,
 * whatever is or isn't on it.
 *
 * Exit codes: 0 nothing left to serve (the client closed the fs channel,
 * or the disk went away); 1 the volume can't be served and a restart may
 * help (a disk error); FAT_EXIT_NO_VOLUME there is no FAT volume fat can
 * serve on the partition (and nothing to format): a restart would find
 * the same. */
#pragma once

#include <jam/startup.h>

#define FAT_SR_BLOCK (SR_USER + 0)
#define FAT_SR_SERVE (SR_USER + 2)   /* SR_DRIVER(DR_SERVE) */
#define FAT_SR_CTL   (SR_USER + 3)

/* SR_STATE's size: room for fat's layout (svcstate_size of it, about
 * 1.3 MiB) and its growth. Pages are committed only as fat uses them, so
 * room to spare costs nothing; svcstate_open refuses a smaller VMO. */
#define FAT_STATE_SIZE (4ull << 20)

#define FAT_ARG_FORMAT     "format-if-blank"
#define FAT_EXIT_NO_VOLUME 2
