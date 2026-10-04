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
 *                  SVCSTATE_SERVICE_RIGHTS. An instance that finds a dead
 *                  one's state in it carries on from it: the volume as it
 *                  was (no mount), its open files and views, the request
 *                  in progress finished exactly once (user/services/fat/
 *                  adopt.c). Without it fat makes a state of its own.
 *   SR_KEEP        optional: its end of the keep channel (<keep.h>); devmgr
 *                  is the keeper, fat puts every open file's and view's
 *                  handles with it, and a successor is handed them back.
 *
 * devmgr also starts one fat with nothing but SR_STANDBY, the warm spare
 * (<svcstate.h>), and promotes it with the handles above when a mount's
 * fat dies.
 *
 * argv[1], if given, names the instance in its log lines ("/data"). The
 * words after it may be, in any order: FAT_ARG_FORMAT (below), and for an
 * instance that replaces one that ended, FAT_ARG_KILLED (a deliberate
 * kill) or FAT_ARG_CRASHED (any other end): only a crash counts against
 * the request in progress (docs/M11.6-PLAN.md, Q1: a request in progress
 * at two crashes is answered ERR_IO and dropped). A test that starts fat
 * itself may add test powers (user/services/fat/test.c).
 *
 * Formatting is off unless a word is FAT_ARG_FORMAT. Only then, and only
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
 * the same; FAT_EXIT_TEST a test power ended it (test.c). */
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
#define FAT_ARG_KILLED     "killed"
#define FAT_ARG_CRASHED    "crashed"
#define FAT_EXIT_NO_VOLUME 2
#define FAT_EXIT_TEST      3
